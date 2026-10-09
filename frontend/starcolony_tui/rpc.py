"""JSON-RPC over stdio 客户端：子进程管理 + 行协议编解码 + 超时/退出/异常处理。

严格实现 docs/PROTOCOL.md §1（传输层）、§2（两种失败必须区分）：

* 分帧：**一行一个 JSON 对象**（newline-delimited，UTF-8）；请求走子进程 stdin，
  响应走子进程 stdout，严格请求-响应配对；服务端**绝不主动推送**。
* stderr 只是调试信息：前端忽略，但用环形缓冲收集尾部若干行，便于出错时排错。
* 空行不是请求：**客户端不得发送空行**。
* 业务失败（``result.ok=false``）**不是异常**：由 ``CommandResult`` 承载，UI 自行处理；
  只有协议错误（``error`` 对象 / 非法 JSON）与传输错误（超时 / 崩溃 / EOF）才抛异常。

线程模型：一个后台线程阻塞读 stdout，把每一行推入 ``queue.Queue``；主调线程用
``_read_matching`` 按请求 ``id`` 取回对应响应（容忍迟到的旧响应，自愈错位）。
同步 API 供测试直接调用；``a*`` 变体用 ``asyncio.to_thread`` 包装，避免阻塞 Textual 事件循环。
"""

from __future__ import annotations

import asyncio
import json
import os
import queue
import subprocess
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

# ============================================================
#  异常
# ============================================================


class RpcError(Exception):
    """所有前端侧 RPC 失败的基类（便于 UI 统一捕获）。"""


class RpcStartupError(RpcError):
    """子进程无法启动（二进制缺失 / 不可执行）。"""


class RpcTransportError(RpcError):
    """传输层失败：超时、进程崩溃、EOF、写管道断开。"""


class RpcProtocolError(RpcError):
    """协议层失败：非法 JSON、缺少 result、JSON-RPC error 对象。"""


# ============================================================
#  结果对象
# ============================================================

@dataclass
class CommandResult:
    """``command`` / ``save`` / ``load`` 的统一返回体。

    ``ok`` 为 False 时是**正常业务结果**（如资源不足），不代表协议出错。
    UI **必须**用 ``code`` 判定成败，禁止解析 ``text``（PROTOCOL §4.6）。
    """

    ok: bool
    code: str
    text: str
    args: list[str] = field(default_factory=list)
    log: list[dict[str, Any]] = field(default_factory=list)
    snapshot: Optional[dict[str, Any]] = None
    raw: dict[str, Any] = field(default_factory=dict)

    @classmethod
    def from_json(cls, obj: dict[str, Any]) -> "CommandResult":
        inner = obj.get("result") if isinstance(obj.get("result"), dict) else {}
        snap = obj.get("snapshot")
        log = obj.get("log")
        args = inner.get("args")
        return cls(
            ok=bool(obj.get("ok", False)),
            code=str(inner.get("code", "Ok")),
            text=str(inner.get("text", "")),
            args=[str(a) for a in args] if isinstance(args, list) else [],
            log=[e for e in log if isinstance(e, dict)] if isinstance(log, list) else [],
            snapshot=snap if isinstance(snap, dict) else None,
            raw=obj,
        )


# ============================================================
#  二进制路径解析
# ============================================================

def default_binary() -> str:
    """定位 ``starcolony-rpc``：优先环境变量 ``STARCOLONY_RPC``，否则仓库 ``build/``。"""
    env = os.environ.get("STARCOLONY_RPC")
    if env:
        return env
    # .../frontend/starcolony_tui/rpc.py -> parents[2] 是仓库根
    root = Path(__file__).resolve().parents[2]
    return str(root / "build" / "starcolony-rpc")


class _Eof:
    """stdout 关闭的哨兵对象。"""


_EOF = _Eof()


# ============================================================
#  客户端
# ============================================================

class RpcClient:
    """``starcolony-rpc`` 子进程的同步客户端（带 async 包装）。"""

    def __init__(
        self,
        binary: Optional[str] = None,
        extra_args: Optional[list[str]] = None,
        timeout: float = 15.0,
        stderr_tail: int = 60,
    ) -> None:
        self.binary = binary or default_binary()
        self.extra_args = list(extra_args or [])
        self.timeout = float(timeout)
        self._proc: Optional[subprocess.Popen[str]] = None
        self._q: "queue.Queue[Any]" = queue.Queue()
        self._lock = threading.Lock()
        self._next_id = 1
        self._reader: Optional[threading.Thread] = None
        self._err_reader: Optional[threading.Thread] = None
        self._stderr_tail: deque[str] = deque(maxlen=int(stderr_tail))
        self._stray: deque[dict[str, Any]] = deque(maxlen=16)
        self._started = False
        # 服务端是否支持 preview_build（首次收到 -32601 后置位，避免重复试探）
        self._preview_unsupported = False

    # ---------------- 生命周期 ----------------

    def start(self) -> None:
        """启动子进程并起后台读取线程。失败抛 :class:`RpcStartupError`。"""
        if self._proc is not None and self._proc.poll() is None:
            return
        if not os.path.exists(self.binary):
            raise RpcStartupError(
                f"找不到服务端二进制：{self.binary}\n"
                f"（请先构建：cd 仓库根 && cmake --build build 或 make）"
            )
        if not os.access(self.binary, os.X_OK):
            raise RpcStartupError(f"服务端二进制不可执行：{self.binary}")

        try:
            self._proc = subprocess.Popen(
                [self.binary, *self.extra_args],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,  # 行缓冲
            )
        except OSError as exc:  # pragma: no cover - 环境相关
            raise RpcStartupError(f"无法启动服务端 {self.binary}：{exc}") from exc

        self._q = queue.Queue()
        self._stray.clear()
        self._stderr_tail.clear()
        self._started = True
        self._reader = threading.Thread(target=self._pump_stdout, name="sc-rpc-stdout", daemon=True)
        self._reader.start()
        self._err_reader = threading.Thread(target=self._pump_stderr, name="sc-rpc-stderr", daemon=True)
        self._err_reader.start()

    @property
    def alive(self) -> bool:
        return self._proc is not None and self._proc.poll() is None

    @property
    def pid(self) -> Optional[int]:
        return self._proc.pid if self._proc is not None else None

    def stderr_text(self) -> str:
        """返回服务端 stderr 的尾部若干行（用于出错提示）。"""
        return "\n".join(self._stderr_tail)

    def _pump_stdout(self) -> None:
        proc = self._proc
        if proc is None or proc.stdout is None:
            self._q.put(_EOF)
            return
        try:
            for line in proc.stdout:
                self._q.put(line)
        except (ValueError, OSError):  # pragma: no cover - 关闭竞态
            pass
        finally:
            self._q.put(_EOF)

    def _pump_stderr(self) -> None:
        proc = self._proc
        if proc is None or proc.stderr is None:
            return
        try:
            for line in proc.stderr:
                self._stderr_tail.append(line.rstrip("\n"))
        except (ValueError, OSError):  # pragma: no cover
            pass

    def close(self, timeout: float = 2.0) -> None:
        """回收子进程：先礼貌 shutdown，再 terminate，最后 kill，绝不留下僵尸。"""
        proc = self._proc
        if proc is None:
            return
        with self._lock:
            self._started = False
        if proc.poll() is None:
            # 尽力发一条 shutdown（不等响应，也容忍写失败）
            try:
                if proc.stdin is not None:
                    proc.stdin.write(json.dumps(
                        {"jsonrpc": "2.0", "id": "shutdown", "method": "shutdown"}
                    ) + "\n")
                    proc.stdin.flush()
            except (BrokenPipeError, OSError, ValueError):
                pass
            try:
                proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                proc.terminate()
                try:
                    proc.wait(timeout=1.0)
                except subprocess.TimeoutExpired:  # pragma: no cover - 顽固进程
                    proc.kill()
                    try:
                        proc.wait(timeout=1.0)
                    except subprocess.TimeoutExpired:
                        pass
        for stream in (proc.stdin, proc.stdout, proc.stderr):
            try:
                if stream is not None:
                    stream.close()
            except OSError:  # pragma: no cover
                pass
        for thread in (self._reader, self._err_reader):
            if thread is not None and thread.is_alive():
                thread.join(timeout=1.0)
        self._proc = None

    def __enter__(self) -> "RpcClient":
        self.start()
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    # ---------------- 低层编解码 ----------------

    def _write_line(self, obj: dict[str, Any]) -> None:
        proc = self._proc
        if proc is None or proc.stdin is None or proc.poll() is not None:
            raise RpcTransportError("服务端进程不可用（未启动或已退出）")
        line = json.dumps(obj, ensure_ascii=False)
        if not line.strip():
            raise RpcProtocolError("内部错误：拒绝发送空行")  # §1.1 客户端不得发空行
        try:
            proc.stdin.write(line + "\n")
            proc.stdin.flush()
        except (BrokenPipeError, OSError, ValueError) as exc:
            raise RpcTransportError(f"写入服务端失败（进程可能已崩溃）：{exc}") from exc

    def _read_matching(self, request_id: Any, deadline: float) -> dict[str, Any]:
        """按 id 取回响应；跳过迟到的旧响应（自愈错位）。超时/EOF 抛传输错误。"""
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RpcTransportError(
                    f"服务端无响应（超时 {self.timeout:g}s）"
                    + (f"\nstderr 尾部：\n{self.stderr_text()}" if self._stderr_tail else "")
                )
            try:
                item = self._q.get(timeout=min(remaining, 0.5))
            except queue.Empty:
                if not self.alive and self._q.empty():
                    raise RpcTransportError(
                        "服务端进程已退出"
                        + (f"\nstderr 尾部：\n{self.stderr_text()}" if self._stderr_tail else "")
                    )
                continue

            if isinstance(item, _Eof):
                raise RpcTransportError(
                    "服务端关闭了输出流（进程已退出）"
                    + (f"\nstderr 尾部：\n{self.stderr_text()}" if self._stderr_tail else "")
                )

            text = item.strip()
            if not text:
                continue  # 空行按 §1.1 静默忽略
            try:
                obj = json.loads(text)
            except json.JSONDecodeError as exc:
                raise RpcProtocolError(f"服务端返回非法 JSON：{text[:200]!r}（{exc}）") from exc
            if not isinstance(obj, dict):
                raise RpcProtocolError(f"服务端响应不是 JSON 对象：{text[:200]!r}")

            if obj.get("id") != request_id:
                # 迟到/无关响应：记录后跳过，避免把旧响应当成新请求的结果
                self._stray.append(obj)
                continue

            if "error" in obj:
                err = obj.get("error")
                if isinstance(err, dict):
                    raise RpcProtocolError(
                        f"协议错误 [{err.get('code')}]：{err.get('message', '')}"
                    )
                raise RpcProtocolError(f"协议错误：{err!r}")
            if "result" not in obj:
                raise RpcProtocolError(f"响应缺少 result：{text[:200]!r}")
            result = obj["result"]
            if not isinstance(result, dict):
                # 允许标量 result（本契约都是对象），包一层以便上层统一处理
                return {"value": result}
            return result

    def call(self, method: str, params: Optional[dict[str, Any]] = None,
             timeout: Optional[float] = None) -> dict[str, Any]:
        """同步发起一次请求，返回 JSON-RPC ``result`` 对象。

        只抛 :class:`RpcError` 子类；业务失败不在这里表达。
        """
        if self._proc is None:
            raise RpcTransportError("服务端尚未启动")
        with self._lock:
            request_id = self._next_id
            self._next_id += 1
            req: dict[str, Any] = {"jsonrpc": "2.0", "id": request_id, "method": method}
            if params is not None:
                req["params"] = params
            self._write_line(req)
            deadline = time.monotonic() + float(timeout if timeout is not None else self.timeout)
            return self._read_matching(request_id, deadline)

    # ---------------- 高层方法（§4）----------------

    def ping(self) -> bool:
        """§4.1，确认子进程已就绪。"""
        res = self.call("ping")
        return bool(res.get("ok")) and bool(res.get("pong", True))

    def content_info(self) -> dict[str, Any]:
        """§4.2，启动时拉一次并缓存。"""
        return self.call("content_info")

    def new_game(self, seed: Optional[int] = None, name: Optional[str] = None) -> dict[str, Any]:
        """§4.2，返回里直接带完整 snapshot。"""
        params: dict[str, Any] = {}
        if seed is not None:
            params["seed"] = int(seed)
        if name is not None:
            params["name"] = name
        return self.call("new_game", params or None)

    def snapshot(self) -> dict[str, Any]:
        """§4.4，纯查询，不消耗随机数。"""
        return self.call("snapshot")

    def command(self, action: str, **params: Any) -> CommandResult:
        """§4.6，唯一的变更入口。返回体同时含 result/log/snapshot。"""
        payload: dict[str, Any] = {"action": action}
        payload.update({k: v for k, v in params.items() if v is not None})
        res = self.call("command", payload)
        return CommandResult.from_json(res)

    def save(self, path: Optional[str] = None) -> CommandResult:
        res = self.call("save", ({"path": path} if path else None))
        return CommandResult.from_json(res)

    def load(self, path: Optional[str] = None) -> CommandResult:
        res = self.call("load", ({"path": path} if path else None))
        return CommandResult.from_json(res)

    @property
    def supports_preview_build(self) -> bool:
        """服务端是否已实现 ``preview_build``（未知则视为不支持）。"""
        return not self._preview_unsupported

    def preview_build(self, key: str, x: int, y: int) -> Optional[dict[str, Any]]:
        """只读查询：某建筑在某地块能否建造、是否付得起。

        **尽力而为**：服务端未实现（``-32601``）时静默返回 ``None`` 并记住不支持；
        任何协议/传输错误也一律返回 ``None``，绝不向上抛——预览只是锦上添花，
        不能让建造流程因此弹错误框。
        """
        if self._preview_unsupported:
            return None
        try:
            return self.call("preview_build", {"key": key, "x": int(x), "y": int(y)})
        except RpcProtocolError as exc:
            if "-32601" in str(exc):
                self._preview_unsupported = True
            return None
        except RpcError:
            return None

    # ---------------- async 包装（供 Textual 使用）----------------

    async def acall(self, method: str, params: Optional[dict[str, Any]] = None,
                    timeout: Optional[float] = None) -> dict[str, Any]:
        return await asyncio.to_thread(self.call, method, params, timeout)

    async def aping(self) -> bool:
        return await asyncio.to_thread(self.ping)

    async def acontent_info(self) -> dict[str, Any]:
        return await asyncio.to_thread(self.content_info)

    async def anew_game(self, seed: Optional[int] = None, name: Optional[str] = None) -> dict[str, Any]:
        return await asyncio.to_thread(self.new_game, seed, name)

    async def asnapshot(self) -> dict[str, Any]:
        return await asyncio.to_thread(self.snapshot)

    async def acommand(self, action: str, **params: Any) -> CommandResult:
        return await asyncio.to_thread(self.command, action, **params)

    async def asave(self, path: Optional[str] = None) -> CommandResult:
        return await asyncio.to_thread(self.save, path)

    async def aload(self, path: Optional[str] = None) -> CommandResult:
        return await asyncio.to_thread(self.load, path)

    async def apreview_build(self, key: str, x: int, y: int) -> Optional[dict[str, Any]]:
        return await asyncio.to_thread(self.preview_build, key, x, y)

    async def aclose(self, timeout: float = 2.0) -> None:
        await asyncio.to_thread(self.close, timeout)


__all__ = [
    "RpcError",
    "RpcStartupError",
    "RpcTransportError",
    "RpcProtocolError",
    "CommandResult",
    "RpcClient",
    "default_binary",
]
