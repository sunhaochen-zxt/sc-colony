"""待决事件弹窗：数字键或「上下 + 回车」选择选项。

数据来源：契约 §4.4.1 的 ``snapshot.pending``（``kind`` / ``title`` / ``text`` / ``options``）。
选项下标 1 基 —— ``options[0]`` 对应 ``command{action:"answer", option:1}``。

``options`` 为空时（极端兜底）退化为「按数字键直接提交选项编号 1-9」。
"""

from __future__ import annotations

from typing import Optional

from rich.text import Text
from textual.binding import Binding
from textual.containers import Vertical
from textual.screen import ModalScreen
from textual.widgets import ListItem, ListView, Static


class EventScreen(ModalScreen[Optional[int]]):
    """待决事件模态。``dismiss(n)`` 返回选项号（1 起），``None`` 表示稍后处理。"""

    BINDINGS = [Binding("escape", "later", "稍后处理")]

    DEFAULT_CSS = """
    EventScreen { align: center middle; }
    EventScreen > Vertical {
        width: 70; height: auto; max-height: 80%;
        border: heavy $warning; background: $surface; padding: 1 2;
    }
    EventScreen #event-title { text-style: bold; color: $warning; height: auto; }
    EventScreen #event-text { height: auto; margin-bottom: 1; }
    EventScreen #event-hint { color: $text-muted; height: auto; margin-top: 1; }
    EventScreen ListView { height: auto; max-height: 12; }
    """

    def __init__(self, title: str, text: str, options: Optional[list[str]] = None, icon: str = "◇") -> None:
        super().__init__()
        self._title = title
        self._text = text
        self._options: Optional[list[str]] = list(options) if options else None
        self._icon = icon

    def compose(self):
        with Vertical():
            yield Static(Text(f"{self._icon} {self._title}", style="bold yellow"), id="event-title")
            yield Static(Text(self._text), id="event-text", markup=False)
            if self._options:
                yield ListView(*[
                    ListItem(Static(Text(f"{i + 1}. {opt}"), markup=False))
                    for i, opt in enumerate(self._options)
                ], id="event-options")
            else:
                yield Static("按数字键直接选择选项编号（1-9）")
            yield Static("↑↓ 选择 · Enter 确认 · 数字键直选 · Esc 稍后处理", id="event-hint")

    def on_mount(self) -> None:
        if self._options:
            self.query_one("#event-options", ListView).focus()

    # ---------------- 选择 ----------------

    @property
    def option_count(self) -> int:
        return len(self._options) if self._options else 0

    def choose(self, option: int) -> None:
        """提交选项号（1 起）。"""
        if option < 1:
            return
        if self._options is not None and option > len(self._options):
            return
        self.dismiss(option)

    def action_later(self) -> None:
        self.dismiss(None)

    # ---------------- 事件 ----------------

    def on_list_view_selected(self, event: ListView.Selected) -> None:
        event.stop()
        idx = event.list_view.index
        if idx is not None:
            self.choose(idx + 1)

    async def on_key(self, event) -> None:  # type: ignore[no-untyped-def]
        key = event.key
        if len(key) == 1 and key.isdigit() and key != "0":
            event.stop()
            event.prevent_default()
            self.choose(int(key))
