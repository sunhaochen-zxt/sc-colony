"""建筑列表面板：上下选中，可拆除/开关/设优先（动作由主屏统一派发）。

每座建筑显示：字形、名称、编号、坐标、已分配工人与状态（在建/受损/关闭/空转）。
队列与工人需求属引擎规则，前端只展示协议里有的字段（``assigned`` / 状态位）。
"""

from __future__ import annotations

from typing import Optional

from rich.text import Text
from textual.containers import Vertical
from textual.message import Message
from textual.widgets import ListView, ListItem, Static

from ..i18n import col_style
from ..models import BuildingState, ContentInfo, Snapshot

_STATUS_STYLE = {
    "ok": "green",
    "building": "yellow",
    "damaged": "red",
    "off": "bright_black",
    "dead": "bright_black",
}
_STATUS_LABEL = {
    "ok": "正常",
    "building": "在建",
    "damaged": "受损",
    "off": "已关闭",
    "dead": "已拆除",
}


class BuildingPanel(Vertical):
    """建筑列表。选中项通过消息上报，动作由主屏（app）执行。"""

    DEFAULT_CSS = """
    BuildingPanel { height: 1fr; }
    BuildingPanel #building-head { height: 1; color: $text-muted; padding: 0 1; }
    BuildingPanel #building-list { height: 1fr; }
    """

    class SelectionChanged(Message):
        """高亮项变化。"""

        def __init__(self, building_id: int) -> None:
            self.building_id = building_id
            super().__init__()

    class InspectRequested(Message):
        """回车请求查看某建筑详情。"""

        def __init__(self, building_id: int) -> None:
            self.building_id = building_id
            super().__init__()

    def __init__(self, **kwargs) -> None:
        super().__init__(**kwargs)
        self._ids: list[int] = []
        self._content: Optional[ContentInfo] = None
        self._snapshot: Optional[Snapshot] = None

    def compose(self):
        yield Static("建筑", id="building-head")
        yield ListView(id="building-list")

    @property
    def selected_id(self) -> Optional[int]:
        """当前高亮建筑的 id；列表为空返回 ``None``。"""
        lv = self.query_one("#building-list", ListView)
        idx = lv.index
        if idx is None or not (0 <= idx < len(self._ids)):
            return None
        return self._ids[idx]

    def _item_for(self, b: BuildingState, content: ContentInfo) -> ListItem:
        return ListItem(Static(self._item_text(b, content), markup=False))

    def _item_text(self, b: BuildingState, content: ContentInfo) -> Text:
        """构造某建筑的一行（供 :meth:`_item_for` 与测试共用）。"""
        info = content.building_by_index(b.type)
        name = info.name if info else b.type_key
        glyph = info.glyph_upper if info else "?"
        color = col_style(info.color) if info else ""
        assigned = self._snapshot.assigned_for(b.id) if self._snapshot else b.assigned
        need = b.worker_need  # 真实字段（可能为 None，回退旧行为）

        t = Text(no_wrap=True)
        t.append(" ")
        t.append(glyph, style=(color or "white"))
        t.append(" ")
        t.append(name, style=(color or "white"))
        t.append(f" #{b.id}", style="bright_black")
        t.append(f" ({b.x},{b.y})", style="bright_black")

        # 工人显示：有真实 workerNeed 就显示 已分配/需求 并对不足给出可见告警
        if need is None:
            t.append(f"  工人 {assigned}", style="bright_black")
        elif need == 0:
            t.append("  无需工人", style="bright_black")
        else:
            t.append(f"  工人 {assigned}/{need}",
                     style=("bold yellow" if assigned < need else "bright_black"))
        t.append("  ")

        status = b.status_text
        style = _STATUS_STYLE.get(b.status_key, "")
        if b.status_key == "ok":
            if need is not None and need > 0 and assigned < need:
                status = "空转(缺人)" if assigned == 0 else "人手不足"
                style = "bold red" if assigned == 0 else "bold yellow"
            elif need is None and info is not None and info.workers > 0 and assigned == 0:
                # 回退路径（服务端未提供 workerNeed）：近似提示
                status, style = "空转(缺人)", "yellow"
        t.append(status, style=style)
        return t

    async def set_data(self, snapshot: Snapshot, content: ContentInfo) -> None:
        """重建列表（保持原选中项尽量不跳）。"""
        self._content = content
        self._snapshot = snapshot
        keep = self.selected_id
        alive = [b for b in snapshot.buildings if b.alive]
        self._ids = [b.id for b in alive]

        head = self.query_one("#building-head", Static)
        head.update(
            f"建筑 {len(alive)} 座  ·  ↑↓ 选择 · d 拆除 · t 开关 · f 设优先 · Enter 详情"
        )

        lv = self.query_one("#building-list", ListView)
        await lv.clear()
        if alive:
            await lv.extend([self._item_for(b, content) for b in alive])
            if keep is not None and keep in self._ids:
                lv.index = self._ids.index(keep)
            else:
                lv.index = 0

    # ---------------- 事件 ----------------

    def on_list_view_highlighted(self, event: ListView.Highlighted) -> None:
        idx = event.list_view.index
        if idx is not None and 0 <= idx < len(self._ids):
            self.post_message(self.SelectionChanged(self._ids[idx]))

    def on_list_view_selected(self, event: ListView.Selected) -> None:
        idx = event.list_view.index
        if idx is not None and 0 <= idx < len(self._ids):
            self.post_message(self.InspectRequested(self._ids[idx]))
