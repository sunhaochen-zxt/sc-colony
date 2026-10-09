"""Textual 应用主类：持有 RPC 客户端与游戏状态，驱动所有界面更新。

设计：
* 单一数据源 —— ``self.content`` / ``self.snapshot`` 来自服务端，界面只读；
* 所有变更都经 :meth:`submit`（唯一变更入口 ``command``），串行化以免状态错乱；
* 业务失败（``ok=false``）用 ``result.code`` 判定，界面用 ``result.text`` 提示（PROTOCOL §4.6）；
* 协议/传输异常统一捕获，转成界面提示，绝不让堆栈刷屏或卡死。
"""

from __future__ import annotations

import argparse
import asyncio
import sys
from collections import deque
from typing import Any, Optional

from rich.text import Text
from textual.app import App

from .i18n import (
    BLOCKING_EVENTS,
    event_definition,
    icon_for_code,
    style_for_code,
)
from .models import BuildPreview, ContentInfo, LogEntry, PendingEvent, Snapshot
from .rpc import CommandResult, RpcClient, RpcError, RpcStartupError
from .screens.dialogs import MessageScreen
from .screens.event import EventScreen
from .screens.surface import SurfaceScreen
from .stdin_watch import StdinEofWatcher
from .widgets import BuildingPanel, LogPanel, MapView, ResourceBar, TechPanel


class StarColonyApp(App[None]):
    """《星际争霸：殖民地》图形化终端前端。"""

    TITLE = "星际争霸：殖民地  STAR COLONY"
    SUB_TITLE = "图形化终端前端（Textual）"

    CSS = """
    Screen { background: $surface; }
    Header { background: $panel; color: $text; }
    Footer { background: $panel; }
    .-text-muted, #status-line { color: $text-muted; }
    """

    def __init__(
        self,
        seed: Optional[int] = None,
        name: Optional[str] = None,
        binary: Optional[str] = None,
        rpc_args: Optional[list[str]] = None,
        timeout: float = 15.0,
    ) -> None:
        super().__init__()
        self.client = RpcClient(binary=binary, extra_args=rpc_args, timeout=timeout)
        self.seed = seed
        self.colony_name = name  # 注意：不要用 self.name —— 那是 Textual App 的属性
        self.content: Optional[ContentInfo] = None
        self.snapshot: Optional[Snapshot] = None
        # 兼容旧服务端的兜底推断（新服务端用 snapshot.pending，见 pending_event）
        self._pending_log: deque[tuple[str, str]] = deque()
        self._feedback: Optional[Text] = None
        self._last_build_errors: dict[str, str] = {}
        self._event_open = False
        self._over_shown = False
        self._rpc_lock: Optional[asyncio.Lock] = None
        # 非交互 stdin 的 EOF 看门（脚本化调用读到 EOF 时按 q 处理）
        self._eof_watcher: Optional[StdinEofWatcher] = None
        self._eof_quitting = False

    # ============================================================
    #  生命周期
    # ============================================================

    async def on_mount(self) -> None:
        self._rpc_lock = asyncio.Lock()
        self._start_eof_watch()
        try:
            await asyncio.to_thread(self.client.start)
            ci = await self.client.acontent_info()
            ng = await self.client.anew_game(self.seed, self.colony_name)
        except RpcStartupError as exc:
            self._fatal("无法启动服务端", str(exc))
            return
        except RpcError as exc:
            self._fatal("连接服务端失败", str(exc))
            return

        self.content = ContentInfo.from_json(ci)
        snap_obj = ng.get("snapshot") if isinstance(ng, dict) else None
        if not isinstance(snap_obj, dict):
            self._fatal("服务端返回异常", "new_game 未返回 snapshot")
            return
        self.snapshot = Snapshot.from_json(snap_obj)
        self.SUB_TITLE = self.snapshot.colony_name or self.SUB_TITLE
        self.rebuild_pending(self.snapshot.log)

        await self.push_screen(SurfaceScreen())  # 等主屏挂载后再填充数据
        await self.refresh_ui(reload_log=True)
        self._maybe_show_event()

    def _fatal(self, title: str, body: str) -> None:
        self.set_feedback(Text(f"✖ {title}", style="bold red"))
        self.push_screen(
            MessageScreen(title, Text(body, style="white"), buttons=["退出"]),
            lambda _label: self.exit(),
        )

    async def on_unmount(self) -> None:
        self.shutdown()

    def shutdown(self) -> None:
        """幂等收尾：停止 stdin 看门线程 + 回收 ``starcolony-rpc`` 子进程。

        正常退出（``q`` / EOF / 弹窗「退出」）与异常退出（``main()`` 的 finally）
        共用同一条路径，保证不会留下孤儿进程。
        """
        self._stop_eof_watch()
        self._close_client()

    def _start_eof_watch(self) -> None:
        """非交互 stdin 读到 EOF 时按 ``q`` 退出，避免脚本化调用挂死。

        Textual 的输入驱动遇到管道 EOF 只会空转、不发事件，所以需要自己判定。
        ``run_test()`` 的 headless 冒烟不参与（其 stdin 与真实运行无关）。
        """
        if self.is_headless or self._eof_watcher is not None:
            return
        watcher = StdinEofWatcher(self._on_stdin_eof)
        if watcher.start():
            self._eof_watcher = watcher

    def _stop_eof_watch(self) -> None:
        watcher = self._eof_watcher
        if watcher is not None:
            self._eof_watcher = None
            watcher.stop()

    def _on_stdin_eof(self) -> None:
        """stdin EOF 回调（运行在后台守护线程）——语义等价于玩家按 ``q``。"""
        if self._eof_quitting:
            return
        self._eof_quitting = True
        try:
            self.call_from_thread(self._exit_on_eof)
        except Exception:
            # App 已不在运行 / 正在退出：无需再退出，也不该把异常抛到守护线程外。
            pass

    def _exit_on_eof(self) -> None:
        self.exit()

    def _close_client(self) -> None:
        try:
            self.client.close()
        except Exception:  # pragma: no cover - 退出兜底
            pass

    # ============================================================
    #  状态访问辅助
    # ============================================================

    def _widget(self, selector: str, expect: type):
        """按选择器在「地表主屏」内取组件；未挂载时返回 ``None``（避免竞态崩溃）。"""
        surf = self._surface()
        if surf is None:
            return None
        try:
            return surf.query_one(selector, expect)
        except Exception:
            return None

    def _surface(self) -> Optional[SurfaceScreen]:
        for screen in reversed(self.screen_stack):
            if isinstance(screen, SurfaceScreen):
                return screen
        return None

    @property
    def surface(self) -> Optional[SurfaceScreen]:
        """地表主屏实例（供界面与测试访问；弹窗打开时栈顶是弹窗，这里仍能取到主屏）。"""
        return self._surface()

    def set_feedback(self, text: Text) -> None:
        self._feedback = text

    @property
    def pending_event(self) -> Optional[PendingEvent]:
        """当前待决事件。

        首选契约 §4.4.1 的 ``snapshot.pending``（权威数据）。仅当连到尚未实现该段的
        旧服务端（``snapshot.pending`` 恒为 null）时，才退化为从日志公告码推断。
        """
        if self.snapshot is not None and self.snapshot.pending is not None:
            return self.snapshot.pending
        if self._pending_log:
            code, title = self._pending_log[0]
            ev = event_definition(code)
            if ev is not None:
                return PendingEvent(code, str(ev["title"]), str(ev["text"]), tuple(ev["options"]))  # type: ignore[arg-type]
            return PendingEvent(code, title or "未知事件",
                                "（旧服务端未提供事件详情，请按数字键选择选项编号 1-9）", ())
        return None

    @property
    def has_pending(self) -> bool:
        return self.pending_event is not None

    @property
    def pending_codes(self) -> list[str]:
        pe = self.pending_event
        return [pe.kind] if pe is not None else []

    # ============================================================
    #  待决事件推断（契约未暴露 pending，见 i18n 说明）
    # ============================================================

    def ingest_log(self, entries) -> None:
        """兜底：从日志序列推断待决事件队列（FIFO，与引擎 pending_ 一致）。"""
        for e in entries:
            code = e.get("code") if isinstance(e, dict) else getattr(e, "code", None)
            if code is None:
                continue
            if code in BLOCKING_EVENTS:
                ev = event_definition(code)
                title = str(ev["title"]) if ev else "未知事件"
                self._pending_log.append((code, title))
            elif code == "LogChoice":
                if self._pending_log:
                    self._pending_log.popleft()

    def rebuild_pending(self, full_log) -> None:
        self._pending_log.clear()
        self.ingest_log(full_log)

    # ============================================================
    #  唯一变更入口
    # ============================================================

    async def submit(self, action: str, **params: Any) -> Optional[CommandResult]:
        """发起一次 ``command``；更新快照/日志/事件队列并刷新界面。"""
        assert self._rpc_lock is not None
        async with self._rpc_lock:
            try:
                res = await self.client.acommand(action, **params)
            except RpcError as exc:
                self.set_feedback(Text(f"⚠ {exc}", style="bold red"))
                self.update_flash()
                return None

            if res.snapshot:
                self.snapshot = Snapshot.from_json(res.snapshot)
            self.ingest_log(res.log)

            if res.ok:
                style = style_for_code(res.code) or "green"
                icon = icon_for_code(res.code)
                msg = res.text or "完成"
                self.set_feedback(Text(f"{icon} {msg}".strip(), style=style))
            elif res.code == "BlockedByPending":
                # §4.4.1：引擎强制拦截（前端即便漏拦也有引擎兜底），提示并弹窗
                self.set_feedback(Text(f"⚠ {res.text}", style="bold yellow"))
            else:
                # 业务失败：用 code 判定，text 提示（PROTOCOL §4.6）
                self.set_feedback(Text(f"✖ {res.text}", style="bold red"))

            self._append_log(res.log)
            await self.refresh_ui()
            self._maybe_show_event()
            return res

    def _append_log(self, entries) -> None:
        if not entries:
            return
        panel = self._widget("#log-panel", LogPanel)
        if panel is None:
            return
        parsed = [LogEntry.from_json(e) if isinstance(e, dict) else e for e in entries]
        panel.append(parsed)

    # ============================================================
    #  游戏动作
    # ============================================================

    def _guard(self) -> bool:
        """返回 True 表示可以执行动作；否则给出提示并阻止。"""
        if self.snapshot is None:
            return False
        if self.snapshot.over:
            self.set_feedback(Text("游戏已经结束，请重开或退出", style="bold yellow"))
            self.update_flash()
            return False
        return True

    async def do_advance(self) -> None:
        if not self._guard():
            return
        pe = self.pending_event
        if pe is not None:
            self.set_feedback(Text(f"⚠ 有事件待决：{pe.title}（按 e 处理）", style="bold yellow"))
            self.update_flash()
            self._maybe_show_event()
            return
        await self.submit("advance")

    async def do_build(self, key: str, x: int, y: int) -> None:
        if not self._guard():
            return
        if self.pending_event is not None:
            self.set_feedback(Text("⚠ 有事件待决，先按 e 处理", style="bold yellow"))
            self.update_flash()
            self._maybe_show_event()
            return
        res = await self.submit("build", key=key, x=x, y=y)
        if res is not None:
            if res.ok:
                self._last_build_errors.pop(key, None)
            else:
                self._last_build_errors = {key: res.text}

    async def do_demolish(self, building_id: Optional[int]) -> None:
        if not self._guard():
            return
        if building_id is None:
            self.set_feedback(Text("请先在「建筑」面板选择一座建筑", style="yellow"))
            self.update_flash()
            return
        await self.submit("demolish", id=building_id)

    async def do_toggle(self, building_id: Optional[int]) -> None:
        if not self._guard():
            return
        if building_id is None:
            self.set_feedback(Text("请先在「建筑」面板选择一座建筑", style="yellow"))
            self.update_flash()
            return
        await self.submit("toggle", id=building_id)

    async def do_focus(self, building_id: Optional[int]) -> None:
        if not self._guard():
            return
        if building_id is None:
            self.set_feedback(Text("请先在「建筑」面板选择一座建筑", style="yellow"))
            self.update_flash()
            return
        await self.submit("focus", id=building_id)

    async def do_research(self, key: Optional[str]) -> None:
        if not self._guard():
            return
        if not key:
            self.set_feedback(Text("请先在「科技」面板选择一项科技", style="yellow"))
            self.update_flash()
            return
        await self.submit("research", key=key)

    async def do_answer(self, option: int) -> None:
        self._event_open = False
        await self.submit("answer", option=option)

    async def do_save(self, path: Optional[str]) -> None:
        assert self._rpc_lock is not None
        async with self._rpc_lock:
            try:
                res = await self.client.asave(path)
            except RpcError as exc:
                self.set_feedback(Text(f"⚠ {exc}", style="bold red"))
                self.update_flash()
                return
            if res.ok:
                self.set_feedback(Text(f"✔ 已存档：{path or 'save.txt'}", style="bold green"))
            else:
                self.set_feedback(Text(f"✖ 存档失败：{res.text}", style="bold red"))
            await self.refresh_ui()

    async def do_load(self, path: Optional[str]) -> None:
        assert self._rpc_lock is not None
        async with self._rpc_lock:
            try:
                res = await self.client.aload(path)
            except RpcError as exc:
                self.set_feedback(Text(f"⚠ {exc}", style="bold red"))
                self.update_flash()
                return
            if res.ok and res.snapshot:
                self.snapshot = Snapshot.from_json(res.snapshot)
                self._pending_log.clear()
                self._event_open = False
                self._over_shown = False
                self._last_build_errors = {}
                self.rebuild_pending(self.snapshot.log)
                self.set_feedback(Text(f"✔ 已读档：{path or 'save.txt'}", style="bold green"))
                await self.refresh_ui(reload_log=True)
            else:
                self.set_feedback(Text(f"✖ 读档失败：{res.text}", style="bold red"))
                self.update_flash()

    async def do_restart(self) -> None:
        assert self._rpc_lock is not None
        async with self._rpc_lock:
            try:
                ng = await self.client.anew_game(None, self.colony_name)
            except RpcError as exc:
                self.set_feedback(Text(f"⚠ {exc}", style="bold red"))
                self.update_flash()
                return
            snap_obj = ng.get("snapshot") if isinstance(ng, dict) else None
            if not isinstance(snap_obj, dict):
                self.set_feedback(Text("✖ 重开失败：服务端未返回 snapshot", style="bold red"))
                self.update_flash()
                return
            self.snapshot = Snapshot.from_json(snap_obj)
            self._pending_log.clear()
            self._event_open = False
            self._over_shown = False
            self._last_build_errors = {}
            self.rebuild_pending(self.snapshot.log)
            self.set_feedback(Text("★ 新的一局开始了，祝好运！", style="bold green"))
            await self.refresh_ui(reload_log=True)

    # ============================================================
    #  界面刷新
    # ============================================================

    async def refresh_ui(self, reload_log: bool = False) -> None:
        if self.content is None or self.snapshot is None:
            return
        mv = self._widget("#map-view", MapView)
        if mv is None:
            return
        mv.set_content(self.content)
        mv.set_snapshot(self.snapshot)

        rb = self._widget("#resource-bar", ResourceBar)
        if rb is not None:
            rb.set_state(self.snapshot, self.content)

        bp = self._widget("#building-panel", BuildingPanel)
        if bp is not None:
            await bp.set_data(self.snapshot, self.content)

        tp = self._widget("#tech-panel", TechPanel)
        if tp is not None:
            await tp.set_data(self.content, self.snapshot)

        lp = self._widget("#log-panel", LogPanel)
        if lp is not None and reload_log:
            lp.reload(self.snapshot.log)

        self.update_cursor_status()
        self.update_flash()
        self._maybe_show_game_over()

    def update_cursor_status(self) -> None:
        surf = self._surface()
        mv = self._widget("#map-view", MapView)
        if surf is None or mv is None:
            return
        surf.set_status(mv.cell_description())

    def update_building_status(self, building_id: int) -> None:
        surf = self._surface()
        if surf is None:
            return
        text = self._building_text(building_id)
        if text is not None:
            surf.set_flash(text)
        else:
            self.update_flash()

    def update_flash(self) -> None:
        surf = self._surface()
        if surf is None:
            return
        parts: list[Text] = []
        snap = self.snapshot
        if snap is not None:
            if snap.over:
                verdict = "撤离成功" if snap.won else "殖民失败"
                parts.append(Text(f"■ 游戏结束（{verdict}）：{snap.end_reason}", style="bold red"))
            pe = self.pending_event
            if pe is not None:
                parts.append(Text(f"⚠ 待决事件：{pe.title}（按 e 处理）", style="bold yellow"))
            if snap.brownout:
                parts.append(Text("⚠ 能源透支：设施半负荷", style="yellow"))
            if snap.starving:
                parts.append(Text("⚠ 食物透支：人口下降", style="red"))
        if self._feedback is not None:
            parts.append(self._feedback)
        if not parts:
            parts.append(Text(
                "空格 推进周期 · 方向键移动 · Enter 建造 · 建筑快捷键建造 · ? 帮助",
                style="bright_black",
            ))
        line = Text()
        for i, part in enumerate(parts):
            if i:
                line.append("   ·   ", style="bright_black")
            line.append_text(part)
        surf.set_flash(line)

    def _building_text(self, building_id: int) -> Optional[Text]:
        if self.snapshot is None or self.content is None:
            return None
        b = self.snapshot.building(building_id)
        if b is None:
            return None
        info = self.content.building_by_index(b.type)
        name = info.name if info else b.type_key
        t = Text()
        t.append("选中 ", style="bright_black")
        t.append(f"{name} #{b.id}", style="bold cyan")
        t.append(f" ({b.x},{b.y})  ", style="bright_black")
        t.append(b.status_text, style="yellow")
        t.append(f"  工人 {self.snapshot.assigned_for(b.id)}", style="bright_black")
        if info is not None:
            t.append(f"  耗能 {info.upkeep}  工期 {info.build_turns}", style="bright_black")
        t.append("   · d 拆除 · t 开关 · f 设优先", style="bright_black")
        return t

    def show_building_detail(self, building_id: int) -> None:
        if self.snapshot is None or self.content is None:
            return
        b = self.snapshot.building(building_id)
        if b is None:
            return
        info = self.content.building_by_index(b.type)
        body = Text()
        body.append(f"{'名称':<6}", style="bright_black")
        body.append(f"{info.name if info else b.type_key}（{b.type_key}）\n", style="white")
        body.append(f"{'位置':<6}", style="bright_black")
        body.append(f"({b.x},{b.y})\n", style="white")
        body.append(f"{'状态':<6}", style="bright_black")
        body.append(b.status_text + "\n", style="yellow")
        body.append(f"{'工人':<6}", style="bright_black")
        body.append(f"{self.snapshot.assigned_for(b.id)}\n", style="white")
        if info is not None:
            body.append(f"{'造价':<6}", style="bright_black")
            body.append(f"金属 {info.cost_metal} / 能源 {info.cost_energy} / 科研 {info.cost_science}\n", style="white")
            body.append(f"{'工期':<6}", style="bright_black")
            body.append(f"{info.build_turns} 周期   耗能 {info.upkeep}\n", style="white")
            body.append(f"{'说明':<6}", style="bright_black")
            body.append(info.desc + "\n", style="bright_white")
        self.push_screen(MessageScreen(f"{info.name if info else b.type_key} #{b.id}", body, buttons=["关闭"]))

    # ============================================================
    #  弹窗编排
    # ============================================================

    def _maybe_show_event(self) -> None:
        if self._event_open or self.pending_event is None:
            return
        self.show_event()

    def show_event(self) -> None:
        pe = self.pending_event
        if pe is None:
            self.set_feedback(Text("当前没有待决事件", style="bright_black"))
            self.update_flash()
            return
        if self._event_open:
            return
        self._event_open = True
        icon = icon_for_code(pe.kind) or "◇"
        options = list(pe.options) if pe.options else None
        self.push_screen(EventScreen(pe.title, pe.text, options, icon), self.on_event_choice)

    def on_event_choice(self, option: Optional[int]) -> None:
        if option is None:
            self._event_open = False
            self.update_flash()
            return
        self.run_worker(self.do_answer(option), group="answer")

    def _maybe_show_game_over(self) -> None:
        snap = self.snapshot
        if snap is None or not snap.over or self._over_shown:
            return
        self._over_shown = True
        title = "★ 撤离成功！" if snap.won else "✖ 殖民失败"
        body = Text(snap.end_reason or "", style="white")
        body.append(f"\n\n周期 {snap.turn}   人口 {snap.pop}/{snap.housing}   士气 {snap.morale}\n", style="bright_white")
        body.append("金属 ", style="bright_black")
        body.append(str(snap.metal), style="white")
        body.append("   能源 ", style="bright_black")
        body.append(str(snap.energy), style="white")
        body.append("   食物 ", style="bright_black")
        body.append(str(snap.food), style="white")
        body.append("   科研 ", style="bright_black")
        body.append(str(snap.science), style="white")
        self.push_screen(MessageScreen(title, body, buttons=["重开", "退出"]), self.on_gameover_choice)

    def on_gameover_choice(self, label: Optional[str]) -> None:
        if label == "重开":
            self.run_worker(self.do_restart(), group="restart")
        elif label == "退出":
            self.exit()

    def on_restart_choice(self, label: Optional[str]) -> None:
        if label == "重开":
            self.run_worker(self.do_restart(), group="restart")

    def on_save_path(self, path: Optional[str]) -> None:
        if path:
            self.run_worker(self.do_save(path), group="file")

    def on_load_path(self, path: Optional[str]) -> None:
        if path:
            self.run_worker(self.do_load(path), group="file")

    # ============================================================
    #  建造菜单
    # ============================================================

    def open_build_menu(self, x: int, y: int) -> None:
        """打开建造菜单（先尽力拉一次 preview_build 预检，再弹菜单）。"""
        self.run_worker(self._open_build_menu(x, y), group="menu")

    async def _open_build_menu(self, x: int, y: int) -> None:
        content, snapshot = self.content, self.snapshot
        if content is None or snapshot is None:
            return
        if self.pending_event is not None:
            self.set_feedback(Text("⚠ 有事件待决，先按 e 处理", style="bold yellow"))
            self.update_flash()
            return

        previews = await self.fetch_build_previews(x, y)
        from .screens.build import BuildMenu  # 延迟导入，避免循环

        def _chosen(key: Optional[str]) -> None:
            if key:
                self.run_worker(self.do_build(key, x, y), group="build")

        self.push_screen(
            BuildMenu(x, y, content, snapshot, self._last_build_errors, previews),
            _chosen,
        )

    async def fetch_build_previews(self, x: int, y: int) -> Optional[dict[str, BuildPreview]]:
        """尽力拉取所有建筑在当前格的 ``preview_build`` 预检结果。

        服务端未实现该方法（``-32601``）或任一查询失败 → 返回 ``None``，
        菜单回退到「不灰显」。**前端不自己实现可建/可负担规则**（契约要求）。
        """
        content = self.content
        if content is None or not content.buildings:
            return None
        if not self.client.supports_preview_build:
            return None
        previews: dict[str, BuildPreview] = {}
        for info in content.buildings:
            try:
                res = await self.client.apreview_build(info.key, x, y)
            except Exception:  # 预览是增强项，任何异常都静默回退
                res = None
            if res is None:
                return None
            previews[info.key] = BuildPreview.from_json(res)
        return previews or None


# ============================================================
#  CLI 入口
# ============================================================

def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="starcolony_tui",
        description="《星际争霸：殖民地》图形化终端前端（Textual）",
    )
    parser.add_argument("--seed", "-s", type=int, default=None, help="随机种子（可复现同一张地图）")
    parser.add_argument("--name", "-n", type=str, default=None, help="殖民地名称（默认「新曙光」）")
    parser.add_argument("--rpc", type=str, default=None, help="starcolony-rpc 可执行文件路径")
    parser.add_argument("--timeout", type=float, default=15.0, help="单次 RPC 调用超时秒数（默认 15）")
    return parser


def main(argv: Optional[list[str]] = None) -> int:
    """程序入口：解析参数、启动 Textual 应用，并确保子进程被回收。"""
    args = build_arg_parser().parse_args(argv)
    app = StarColonyApp(seed=args.seed, name=args.name, binary=args.rpc, timeout=args.timeout)
    try:
        app.run()
    finally:
        app.shutdown()
    return 0


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
