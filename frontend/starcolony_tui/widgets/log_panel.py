"""日志面板：按 ``ActCode`` 上色（危险红/正面绿/警告黄），全量可滚动、不截断。

* ``max_lines=None`` + ``auto_scroll=True``：不截断、自动贴底；
* 每条日志前缀周期号（暗灰），正文用服务端 ``text``（与旧 CLI 逐字一致），
  整行按 ``code`` 的严重度着色，危险/正面条目加粗高亮 —— 即「用 code 驱动配色与高亮」。
"""

from __future__ import annotations

from typing import Iterable

from rich.text import Text
from textual.containers import Vertical
from textual.widgets import RichLog, Static

from ..i18n import is_highlight, style_for_code
from ..models import LogEntry


class LogPanel(Vertical):
    """结构化日志视图。"""

    DEFAULT_CSS = """
    LogPanel { height: 1fr; }
    LogPanel #log-head { height: 1; color: $text-muted; padding: 0 1; }
    LogPanel #log-view { height: 1fr; }
    """

    def __init__(self, **kwargs) -> None:
        super().__init__(**kwargs)
        self._count = 0

    def compose(self):
        yield Static("日志", id="log-head")
        yield RichLog(id="log-view", wrap=True, markup=False, auto_scroll=True, max_lines=None)

    # ---------------- 输出 ----------------

    def _view(self) -> RichLog:
        return self.query_one("#log-view", RichLog)

    def format_entry(self, entry: LogEntry) -> Text:
        """把一条结构化日志渲染成带配色的 :class:`rich.text.Text`。"""
        style = style_for_code(entry.code)
        if is_highlight(entry.code):
            style = (style + " bold") if style else "bold"
        t = Text(no_wrap=False)
        t.append(f"[{entry.turn:>3}] ", style="bright_black")
        t.append(entry.text, style=style)
        return t

    def reload(self, entries: Iterable[LogEntry]) -> None:
        """清空后全量重绘（用于新局 / 读档）。"""
        view = self._view()
        view.clear()
        self._count = 0
        self.append(entries)

    def append(self, entries: Iterable[LogEntry]) -> None:
        """追加若干条日志。"""
        view = self._view()
        for entry in entries:
            view.write(self.format_entry(entry))
            self._count += 1
        head = self.query_one("#log-head", Static)
        head.update(f"日志 {self._count} 条（全量、可滚动）")
