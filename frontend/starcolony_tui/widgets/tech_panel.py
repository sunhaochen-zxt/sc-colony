"""科技面板：科技树、前置关系、成本、已研究状态；Enter/回车下达研究指令。

前置关系与成本全部来自 ``content_info.techs[]``（**不硬编码**）。
已研究来源是 ``snapshot.techs``（key 列表）。
"""

from __future__ import annotations

from typing import Optional

from rich.text import Text
from textual.containers import Vertical
from textual.message import Message
from textual.widgets import ListView, ListItem, Static

from ..models import ContentInfo, Snapshot, TechInfo


class TechPanel(Vertical):
    """科技树列表。"""

    DEFAULT_CSS = """
    TechPanel { height: 1fr; }
    TechPanel #tech-head { height: 1; color: $text-muted; padding: 0 1; }
    TechPanel #tech-list { height: 1fr; }
    """

    class ResearchRequested(Message):
        """请求研究某科技（key 为引擎科技键）。"""

        def __init__(self, key: str) -> None:
            self.key = key
            super().__init__()

    def __init__(self, **kwargs) -> None:
        super().__init__(**kwargs)
        self._keys: list[str] = []
        self._content: Optional[ContentInfo] = None
        self._snapshot: Optional[Snapshot] = None

    def compose(self):
        yield Static("科技", id="tech-head")
        yield ListView(id="tech-list")

    @property
    def selected_key(self) -> Optional[str]:
        lv = self.query_one("#tech-list", ListView)
        idx = lv.index
        if idx is None or not (0 <= idx < len(self._keys)):
            return None
        return self._keys[idx]

    def _state(self, tech: TechInfo) -> tuple[str, str]:
        """返回 (标记文本, 颜色)。"""
        s = self._snapshot
        assert s is not None
        if s.has_tech(tech.key):
            return ("[已研究]", "green")
        if all(s.has_tech(r) for r in tech.req):
            return ("[可研究]", "bright_white")
        return ("[缺前置]", "bright_black")

    def _item_for(self, tech: TechInfo, content: ContentInfo) -> ListItem:
        mark, mark_style = self._state(tech)
        t = Text(no_wrap=True)
        t.append(" ")
        t.append(mark, style=mark_style)
        t.append(" ")
        t.append(tech.key, style="bright_black")
        t.append(" ")
        t.append(tech.name, style=("bold " + mark_style) if mark_style != "bright_black" else mark_style)
        t.append(f"  科研 {tech.cost}", style="cyan")
        if tech.req:
            names = [content.tech(r).name if content.tech(r) else r for r in tech.req]
            t.append("  前置：" + " ".join(names), style="bright_black")
        t.append("\n")
        t.append("      " + tech.desc, style="bright_black")
        return ListItem(Static(t, markup=False))

    async def set_data(self, content: ContentInfo, snapshot: Snapshot) -> None:
        self._content = content
        self._snapshot = snapshot
        keep = self.selected_key
        self._keys = [tech.key for tech in content.techs]

        done = sum(1 for tech in content.techs if snapshot.has_tech(tech.key))
        head = self.query_one("#tech-head", Static)
        head.update(
            f"科技 {done}/{len(content.techs)} 已研究  ·  科研点 {snapshot.science}  ·  "
            f"↑↓ 选择 · Enter 研究"
        )

        lv = self.query_one("#tech-list", ListView)
        await lv.clear()
        if content.techs:
            await lv.extend([self._item_for(tech, content) for tech in content.techs])
            if keep is not None and keep in self._keys:
                lv.index = self._keys.index(keep)
            else:
                lv.index = 0

    # ---------------- 事件 ----------------

    def on_list_view_selected(self, event: ListView.Selected) -> None:
        idx = event.list_view.index
        if idx is not None and 0 <= idx < len(self._keys):
            self.post_message(self.ResearchRequested(self._keys[idx]))
