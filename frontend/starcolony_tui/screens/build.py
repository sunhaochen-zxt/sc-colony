"""建造菜单：列出 content_info 里的全部建筑，供选择在当前光标格建造。

* 造价 / 工期 / 工人 / 耗能 / 字形**全部来自 content_info**，不硬编码；
* 能否建造与是否付得起，**一律以服务端 ``preview_build`` 为准**（前端不重复实现规则）：
  - ``buildable == false`` → 标「地形不符」并显示引擎给的 ``reason``；
  - ``affordable`` 有缺项 → 标「资源不足：金属/能源/科研」；
  - 两者相互独立，可分别灰显。
* 服务端未实现 ``preview_build``（或该次查询失败）时 ``previews`` 为 ``None``：
  菜单**不灰显**（回退到旧行为），只保留「上次尝试失败原因」的展示。
"""

from __future__ import annotations

from typing import Optional

from rich.text import Text
from textual.binding import Binding
from textual.containers import Vertical
from textual.screen import ModalScreen
from textual.widgets import ListItem, ListView, Static

from ..i18n import col_style, terrain_glyph, terrain_name, terrain_style
from ..models import BuildPreview, ContentInfo, Snapshot


class BuildMenu(ModalScreen[Optional[str]]):
    """建筑选择模态。``dismiss(key)`` 返回建筑键，``None`` 表示取消。"""

    BINDINGS = [Binding("escape", "cancel", "取消")]

    DEFAULT_CSS = """
    BuildMenu { align: center middle; }
    BuildMenu > Vertical {
        width: 76; height: auto; max-height: 88%;
        border: round $accent; background: $surface; padding: 1 2;
    }
    BuildMenu #build-title { text-style: bold; color: $accent; height: auto; }
    BuildMenu #build-tile { height: auto; color: $text-muted; }
    BuildMenu #build-hint { color: $text-muted; height: auto; margin-top: 1; }
    BuildMenu ListView { height: auto; max-height: 18; }
    """

    def __init__(
        self,
        x: int,
        y: int,
        content: Optional[ContentInfo],
        snapshot: Optional[Snapshot],
        last_errors: Optional[dict[str, str]] = None,
        previews: Optional[dict[str, BuildPreview]] = None,
    ) -> None:
        super().__init__()
        self._x = x
        self._y = y
        self._content = content
        self._snapshot = snapshot
        self._last_errors = dict(last_errors or {})
        self._previews: dict[str, BuildPreview] = dict(previews or {})
        self._keys: list[str] = []

    # ---------------- 组成 ----------------

    def _tile_line(self) -> Text:
        t = Text()
        t.append(f"选址 ({self._x},{self._y})  ", style="bright_black")
        c, s = self._content, self._snapshot
        tile = s.tile(self._x, self._y, c.map_w) if (c and s) else None
        if tile is None:
            t.append("越界", style="red")
            return t
        t.append(terrain_glyph(tile.terrain), style=terrain_style(tile.terrain) or "white")
        t.append(" " + terrain_name(tile.terrain), style="white")
        if tile.terrain == ord("*"):
            t.append(f"  矿量 {tile.ore} 丰度 {tile.richness}", style="yellow")
        b = s.building(tile.building) if s and tile.building >= 0 else None
        if b is not None:
            t.append("  已有建筑！", style="bold red")
        if s is not None:
            t.append(f"   资源：金属 {s.metal} 能源 {s.energy} 科研 {s.science}", style="bright_black")
        return t

    def _item_text(self, key: str) -> Text:
        """构造某建筑在菜单里的一行（供 :meth:`_item_for` 与测试共用）。"""
        assert self._content is not None
        info = self._content.building(key)
        assert info is not None
        pv = self._previews.get(key)

        # 造价：优先用服务端 preview.cost，缺失时回退 content_info
        cost_metal, cost_energy, cost_science = info.cost_metal, info.cost_energy, info.cost_science
        if pv is not None and pv.cost:
            cost_metal = pv.cost.get("metal", cost_metal)
            cost_energy = pv.cost.get("energy", cost_energy)
            cost_science = pv.cost.get("science", cost_science)

        t = Text(no_wrap=True)
        t.append(f" [{info.glyph_upper}] ", style=col_style(info.color) or "white")
        t.append(f"{info.name:<10}", style=col_style(info.color) or "white")
        t.append(f"({info.key}) ", style="bright_black")
        t.append(f"金属 {cost_metal}  能源 {cost_energy}  科研 {cost_science}", style="bright_white")
        t.append(f"  工期 {info.build_turns}  工人 {info.workers}  耗能 {info.upkeep}", style="bright_black")

        # 标注：preview 权威（两类独立灰显）；无 preview 时回退到「上次失败原因」
        if pv is not None and pv.buildable is False:
            t.append(f"  ✖ {pv.reason or '地形/条件不符'}", style="bold red")
        elif pv is not None and not pv.affordable_all:
            lacking = "、".join(pv.lacking) or "资源"
            t.append(f"  ※ 资源不足：{lacking}", style="yellow")
        else:
            err = self._last_errors.get(key)
            if err:
                t.append(f"  ✖ {err}", style="bold red")
        return t

    def _item_for(self, key: str) -> ListItem:
        return ListItem(Static(self._item_text(key), markup=False))

    def compose(self):
        with Vertical():
            yield Static(f"建造 · {self._content.map_w}×{self._content.map_h} 殖民地", id="build-title")
            yield Static(self._tile_line(), id="build-tile", markup=False)
            items: list[ListItem] = []
            if self._content is not None:
                self._keys = [b.key for b in self._content.buildings]
                items = [self._item_for(key) for key in self._keys]
            yield ListView(*items, id="build-list")
            yield Static("↑↓ 选择 · Enter 建造 · 建筑快捷键直选 · Esc 取消", id="build-hint")

    def on_mount(self) -> None:
        lv = self.query_one("#build-list", ListView)
        if self._keys:
            lv.index = 0
            lv.focus()

    # ---------------- 选择 ----------------

    def _select(self, key: str) -> None:
        self.dismiss(key)

    def on_list_view_selected(self, event: ListView.Selected) -> None:
        event.stop()
        idx = event.list_view.index
        if idx is not None and 0 <= idx < len(self._keys):
            self._select(self._keys[idx])

    def action_cancel(self) -> None:
        self.dismiss(None)

    async def on_key(self, event) -> None:  # type: ignore[no-untyped-def]
        key = event.key
        # 建筑快捷键直选（字形，大小写不敏感）
        content = self._content
        if content is not None and len(key) == 1 and key.isalpha():
            up = key.upper()
            for b in content.buildings:
                if b.glyph_upper == up:
                    event.stop()
                    event.prevent_default()
                    self._select(b.key)
                    return
        # 数字键按序号直选
        if len(key) == 1 and key.isdigit() and key != "0":
            idx = int(key) - 1
            if 0 <= idx < len(self._keys):
                event.stop()
                event.prevent_default()
                self._select(self._keys[idx])
