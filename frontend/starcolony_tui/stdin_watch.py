"""非交互 stdin 的 EOF 看门：stdin 被读完时回调（应用把它当作 ``q`` 处理）。

背景
----
Textual 的 ``LinuxDriver.run_input_thread`` 在读到管道 EOF 时只是 ``break``
跳出内层循环，然后 ``while not self.exit_event.is_set()`` 继续空转——它
**不会**产生任何事件，也不会让 App 退出。于是::

    printf '\\n' | python -m starcolony_tui --seed 42

UI 能正常渲染，但进程永远不退出，脚本 / CI 里只能靠 ``timeout`` 杀掉
（exit=124）。而 ``printf 'q\\n' | ...`` 能正常退出，差别仅仅在最后那个 ``q``。

做法
----
本模块在后台守护线程里判定「真正的 EOF」，**只观察、不读取** stdin，因此不会
和 Textual 的输入线程抢字节，脚本化输入（``printf ' \\n\\n'`` 等）照旧有效。

按 stdin 类型分三种探测策略：

======================  ==========================================
stdin 类型               判定方式
======================  ==========================================
管道 / socket            ``select.poll`` 的 ``POLLHUP``
                        （Linux 仅在「无写端 **且** 缓冲区已空」时上报，
                        故 ``HUP 且非 POLLIN`` 即为真 EOF，且不消费数据）
普通文件                比较 ``os.lseek(fd, 0, SEEK_CUR)`` 与 ``st_size``
                        （与 Textual 共享同一文件偏移，同样不消费数据）
其它（如 ``/dev/null``） 退回「``select`` 可读后 ``os.read``，读到 ``b""`` 即 EOF」
======================  ==========================================

交互终端（stdin 是 tty）永远不会 EOF，一律**不启用**看门——玩家用 ``q`` 退出。
"""

from __future__ import annotations

import os
import select
import stat
import sys
import threading
from typing import Callable, Optional

__all__ = ["StdinEofWatcher"]

# 轮询间隔（秒）。太小会空转，太大则退出有延迟；100ms 兼顾两者。
_DEFAULT_INTERVAL = 0.1

# 「读」策略每次最多探测的字节数（够判定非空即可，不必读完）。
_PROBE_CHUNK = 4096


def _stdin_fd() -> Optional[int]:
    """返回可用于探测的 stdin 文件描述符；交互终端 / 不可用时返回 ``None``。"""
    stdin = sys.__stdin__
    if stdin is None:
        return None
    try:
        if stdin.isatty():
            return None  # 交互终端不会 EOF
        return stdin.fileno()
    except (AttributeError, ValueError, OSError):
        return None


class StdinEofWatcher:
    """在后台线程里等待 stdin EOF，命中后调用一次 ``on_eof``。

    典型用法::

        watcher = StdinEofWatcher(self._on_stdin_eof)
        if watcher.start():
            self._eof_watcher = watcher
        ...
        watcher.stop()

    线程为 daemon，``stop()`` 后即失效；即使忘记 ``stop()`` 也不会阻塞进程退出。
    """

    def __init__(
        self,
        on_eof: Callable[[], None],
        *,
        interval: float = _DEFAULT_INTERVAL,
        fd: Optional[int] = None,
    ) -> None:
        """初始化看门。

        Args:
            on_eof: 判定 EOF 后在**后台线程**里调用一次的回调（需自行做线程安全跳转）。
            interval: 轮询间隔秒数，小于 0.01 会被抬到 0.01。
            fd: 显式指定要观察的文件描述符；``None`` 表示自动取非交互 stdin（测试用）。
        """
        self._on_eof = on_eof
        self._interval = max(0.01, float(interval))
        self._explicit_fd = fd
        self._fd: Optional[int] = None
        self._thread: Optional[threading.Thread] = None
        self._stop = threading.Event()
        self._fired = False

    # ------------------------------------------------------------------
    #  对外接口
    # ------------------------------------------------------------------

    @staticmethod
    def should_watch() -> bool:
        """当前 stdin 是否需要看门（仅非交互输入才需要）。"""
        return _stdin_fd() is not None

    @property
    def fired(self) -> bool:
        """是否已经判定到 EOF 并触发过回调。"""
        return self._fired

    def start(self) -> bool:
        """启动看门线程；无需看门（交互终端）时返回 ``False`` 且不起线程。"""
        fd = self._explicit_fd if self._explicit_fd is not None else _stdin_fd()
        if fd is None:
            return False
        self._fd = fd
        thread = threading.Thread(
            target=self._run, name="starcolony-stdin-eof", daemon=True
        )
        self._thread = thread
        thread.start()
        return True

    def stop(self) -> None:
        """停止看门（幂等）。最多等待一个轮询周期。"""
        self._stop.set()
        thread = self._thread
        if thread is not None and thread.is_alive():
            thread.join(timeout=max(0.2, self._interval * 2))
        self._thread = None

    # ------------------------------------------------------------------
    #  内部实现
    # ------------------------------------------------------------------

    def _run(self) -> None:
        """守护线程主体：循环探测直到命中 EOF 或被停止。"""
        fd = self._fd
        if fd is None:
            return
        try:
            probe = self._pick_probe(fd)
        except OSError:
            return  # stdin 不可用（已关闭等）：交由上层其它路径处理
        try:
            while not self._stop.is_set():
                if probe(fd):
                    self._fire()
                    return
                self._stop.wait(self._interval)
        except (OSError, ValueError):
            return  # 描述符失效：静默退出，不影响主流程

    def _pick_probe(self, fd: int) -> Callable[[int], bool]:
        """按 stdin 的文件类型挑选最合适的探测函数。"""
        try:
            mode = os.fstat(fd).st_mode
        except OSError:
            return self._probe_read
        if stat.S_ISFIFO(mode) or stat.S_ISSOCK(mode):
            return self._make_poll_probe(fd)
        if stat.S_ISREG(mode):
            return self._probe_regular_file
        return self._probe_read

    def _make_poll_probe(self, fd: int) -> Callable[[int], bool]:
        """构造基于 ``select.poll`` 的管道探测（不消费任何数据）。"""
        if not hasattr(select, "poll"):  # 极端平台兜底
            return self._probe_read
        poller = select.poll()
        poller.register(fd, select.POLLIN | select.POLLHUP)
        pollin = select.POLLIN
        pollhup = select.POLLHUP
        timeout_ms = max(1, int(self._interval * 1000))

        def probe(_fd: int) -> bool:
            for _, mask in poller.poll(timeout_ms):
                # 无写端 + 缓冲区已空 => 真 EOF；缓冲区还有数据则不算。
                if mask & pollhup and not (mask & pollin):
                    return True
            return False

        return probe

    def _probe_regular_file(self, fd: int) -> bool:
        """普通文件：共享偏移追上总长度即为 EOF（不消费数据）。"""
        return os.lseek(fd, 0, os.SEEK_CUR) >= os.fstat(fd).st_size

    def _probe_read(self, fd: int) -> bool:
        """兜底：等到可读后读一次，读到 ``b""`` 即 EOF。"""
        try:
            readable, _, _ = select.select([fd], [], [], self._interval)
        except (OSError, ValueError):
            return False
        if not readable:
            return False
        try:
            return os.read(fd, _PROBE_CHUNK) == b""
        except OSError:
            return False

    def _fire(self) -> None:
        """触发回调（只触发一次；回调自身抛错也不影响进程）。"""
        if self._fired:
            return
        self._fired = True
        try:
            self._on_eof()
        except Exception:  # pragma: no cover - 回调异常不应拖垮退出流程
            pass
