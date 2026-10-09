"""地表主屏：资源栏 + 地图 + 右侧面板（建筑/科技/日志/帮助）+ 状态/反馈行。

本屏只负责「布局 + 按键路由 + 消息转发」，所有 RPC 调用与状态更新都交给
:class:`~starcolony_tui.app.StarColonyApp`，保持单一职责。
"""

from __future__ import annotations

from textual.app import ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, Vertical
from textual.screen import Screen
from textual.widgets import Footer, Header, Static, TabbedContent, TabPane

from ..widgets import BuildingPanel, LogPanel, MapView, ResourceBar, TechPanel
from .dialogs import MessageScreen, PromptScreen
from .help import HelpPanel, HelpScreen


class SurfaceScreen(Screen[None]):
    """游戏主界面。"""

    DEFAULT_CSS = """
    SurfaceScreen #body { height: 1fr; }
    SurfaceScreen #right { width: 1fr; }
    SurfaceScreen #status-line { height: 1; padding: 0 1; background: $panel; }
    SurfaceScreen #flash-line { height: 1; padding: 0 1; }
    SurfaceScreen TabbedContent { height: 1fr; }
    SurfaceScreen TabPane { padding: 0; }
    """

    BINDINGS = [
        Binding("space", "advance", "推进周期"),
        Binding("n", "advance", "推进周期", show=False),
        Binding("d", "demolish", "拆除"),
        Binding("x", "demolish", "拆除", show=False),
        Binding("t", "toggle", "开关"),
        Binding("f", "focus_top", "设优先", show=False),
        Binding("r", "research", "研究", show=False),
        Binding("e", "show_event", "事件"),
        Binding("question_mark", "help", "帮助"),
        Binding("f2", "switch('tab-build')", "建筑", show=False),
        Binding("f3", "switch('tab-tech')", "科技", show=False),
        Binding("f4", "switch('tab-log')", "日志", show=False),
        Binding("f5", "switch('tab-help')", "帮助", show=False),
        Binding("s", "save", "存档"),
        Binding("l", "load", "读档"),
        Binding("ctrl+n", "restart", "重开"),
        Binding("q", "quit", "退出"),
        Binding("escape", "focus_map", "地图", show=False),
        Binding("tab", "focus_next", "下一区", show=False),
        Binding("shift+tab", "focus_previous", "上一区", show=False),
    ]

    # ---------------- 组成 ----------------

    def compose(self) -> ComposeResult:
        yield Header(show_clock=False)
        with Vertical(id="main"):
            yield ResourceBar(id="resource-bar")
            with Horizontal(id="body"):
                yield MapView(id="map-view")
                with Vertical(id="right"):
                    with TabbedContent(id="tabs", initial="tab-build"):
                        with TabPane("建筑", id="tab-build"):
                            yield BuildingPanel(id="building-panel")
                        with TabPane("科技", id="tab-tech"):
                            yield TechPanel(id="tech-panel")
                        with TabPane("日志", id="tab-log"):
                            yield LogPanel(id="log-panel")
                        with TabPane("帮助", id="tab-help"):
                            yield HelpPanel(id="help-panel")
            yield Static("", id="status-line")
            yield Static("", id="flash-line")
        yield Footer()

    def on_mount(self) -> None:
        # 防御：App 可能在本屏挂载完成前就退出（如非交互 stdin 立刻 EOF），
        # 此时子组件尚未就位，直接 focus 会抛 NoMatches。
        try:
            self.query_one("#map-view", MapView).focus()
        except Exception:
            pass

    # ---------------- 便捷访问 ----------------

    @property
    def map_view(self) -> MapView:
        return self.query_one("#map-view", MapView)

    @property
    def building_panel(self) -> BuildingPanel:
        return self.query_one("#building-panel", BuildingPanel)

    @property
    def tech_panel(self) -> TechPanel:
        return self.query_one("#tech-panel", TechPanel)

    @property
    def log_panel(self) -> LogPanel:
        return self.query_one("#log-panel", LogPanel)

    @property
    def resource_bar(self) -> ResourceBar:
        return self.query_one("#resource-bar", ResourceBar)

    def set_status(self, text) -> None:
        # 防御：退出过程中子组件可能已卸载，刷新状态行失败不应影响退出。
        try:
            self.query_one("#status-line", Static).update(text)
        except Exception:
            pass

    def set_flash(self, text) -> None:
        try:
            self.query_one("#flash-line", Static).update(text)
        except Exception:
            pass

    # ---------------- 动作 ----------------

    async def action_advance(self) -> None:
        await self.app.do_advance()

    async def action_demolish(self) -> None:
        await self.app.do_demolish(self.building_panel.selected_id)

    async def action_toggle(self) -> None:
        await self.app.do_toggle(self.building_panel.selected_id)

    async def action_focus_top(self) -> None:
        await self.app.do_focus(self.building_panel.selected_id)

    async def action_research(self) -> None:
        await self.app.do_research(self.tech_panel.selected_key)

    def action_show_event(self) -> None:
        self.app.show_event()

    def action_help(self) -> None:
        self.app.push_screen(HelpScreen(self.app.content))

    def action_switch(self, pane_id: str) -> None:
        self.query_one("#tabs", TabbedContent).active = pane_id

    def action_focus_map(self) -> None:
        self.map_view.focus()

    def action_save(self) -> None:
        self.app.push_screen(PromptScreen("存档路径", "save.txt"), self.app.on_save_path)

    def action_load(self) -> None:
        self.app.push_screen(PromptScreen("读档路径", "save.txt"), self.app.on_load_path)

    def action_restart(self) -> None:
        self.app.push_screen(
            MessageScreen("重开一局？", "将以新的随机种子开始新游戏，当前进度会丢失。",
                          buttons=["重开", "取消"]),
            self.app.on_restart_choice,
        )

    def action_quit(self) -> None:
        self.app.exit()

    # ---------------- 动态建筑快捷键（取自 content 字形，不硬编码）----------------

    async def on_key(self, event) -> None:  # type: ignore[no-untyped-def]
        key = event.key
        content = self.app.content
        if content is not None and len(key) == 1 and key.isalpha() and key.isupper():
            info = next((b for b in content.buildings if b.glyph_upper == key), None)
            if info is not None:
                event.stop()
                event.prevent_default()
                await self.app.do_build(info.key, *self.map_view.cursor)
                return
        if key in ("?", "question_mark"):
            event.stop()
            event.prevent_default()
            self.action_help()

    # ---------------- 子组件消息 ----------------

    def on_map_view_cursor_moved(self, event: MapView.CursorMoved) -> None:
        self.app.update_cursor_status()

    def on_map_view_build_requested(self, event: MapView.BuildRequested) -> None:
        self.app.open_build_menu(event.x, event.y)

    def on_building_panel_selection_changed(self, event: BuildingPanel.SelectionChanged) -> None:
        self.app.update_building_status(event.building_id)

    def on_building_panel_inspect_requested(self, event: BuildingPanel.InspectRequested) -> None:
        self.app.show_building_detail(event.building_id)

    def on_tech_panel_research_requested(self, event: TechPanel.ResearchRequested) -> None:
        self.run_worker(self.app.do_research(event.key), group="rpc", exclusive=True)


__all__ = ["SurfaceScreen"]
