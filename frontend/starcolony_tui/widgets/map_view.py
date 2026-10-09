"""地表地图组件：方向键移动光标，实时显示当前格信息，Enter 进入建造。

渲染要点：
* 建筑字形/颜色来自 ``content_info.buildings[]``（**不硬编码**）；
* 地形字形直接由引擎下发的字符编码 ``chr(code)`` 还原（**不硬编码字形表**），
  颜色/名字只是显示偏好（``i18n``）；
* 每个格子固定 2 个终端列宽，光标格整体反白；大/小终端下用视口平移保证光标可见；
* 中文宽度交给 Textual 处理（本组件只输出单宽 ASCII 字形 + 中文仅出现在图例，
  图例用 Textual 自身的宽度处理，不会让地图错位）。
"""

from __future__ import annotations

from typing import Optional

from rich.text import Text
from textual.binding import Binding
from textual.message import Message
from textual.widget import Widget

from ..i18n import col_style, terrain_glyph, terrain_name, terrain_style
from ..models import ContentInfo, Snapshot

CELL_W = 2
GUTTER = 3


class MapView(Widget):
    """可聚焦的地图。方向键移动光标；Enter 请求建造。"""

    can_focus = True

    DEFAULT_CSS = """
    MapView {
        height: 1fr;
        padding: 0 1;
    }
    MapView:focus {
        border-left: thick $accent;
    }
    """

    BINDINGS = [
        Binding("up", "move('up')", "上移", show=False, priority=True),
        Binding("down", "move('down')", "下移", show=False, priority=True),
        Binding("left", "move('left')", "左移", show=False, priority=True),
        Binding("right", "move('right')", "右移", show=False, priority=True),
        Binding("enter", "open_build", "建造"),
        Binding("g", "center_hq", "回到指挥中心", show=False),
    ]

    # ---------------- 消息 ----------------

    class CursorMoved(Message):
        """光标移动到新格。"""

        def __init__(self, x: int, y: int) -> None:
            self.x = x
            self.y = y
            super().__init__()

    class BuildRequested(Message):
        """请求在当前光标格打开建造菜单。"""

        def __init__(self, x: int, y: int) -> None:
            self.x = x
            self.y = y
            super().__init__()

    # ---------------- 生命周期 ----------------

    def __init__(self, **kwargs) -> None:
        super().__init__(**kwargs)
        self._content: Optional[ContentInfo] = None
        self._snapshot: Optional[Snapshot] = None
        self.cx = 0
        self.cy = 0
        self.off_x = 0
        self.off_y = 0

    def set_content(self, content: ContentInfo) -> None:
        self._content = content
        # 视口宽度按地图尺寸自适应（不硬编码 18 列）
        self.styles.width = content.map_w * CELL_W + GUTTER + 4
        self.cx = min(self.cx, max(0, content.map_w - 1))
        self.cy = min(self.cy, max(0, content.map_h - 1))
        self.refresh()

    def set_snapshot(self, snapshot: Snapshot) -> None:
        self._snapshot = snapshot
        self._clamp()
        self.refresh()

    # ---------------- 游标 ----------------

    @property
    def cursor(self) -> tuple[int, int]:
        return (self.cx, self.cy)

    def _clamp(self) -> None:
        c = self._content
        if c is None or c.map_w <= 0:
            return
        self.cx = max(0, min(self.cx, c.map_w - 1))
        self.cy = max(0, min(self.cy, c.map_h - 1))

    def move_cursor(self, dx: int, dy: int) -> None:
        """相对移动光标（供键盘与测试直接调用）。"""
        self.cx += dx
        self.cy += dy
        self._clamp()
        self.refresh()
        self.post_message(self.CursorMoved(self.cx, self.cy))

    def set_cursor(self, x: int, y: int) -> None:
        self.cx, self.cy = x, y
        self._clamp()
        self.refresh()
        self.post_message(self.CursorMoved(self.cx, self.cy))

    def action_move(self, direction: str) -> None:
        delta = {"up": (0, -1), "down": (0, 1), "left": (-1, 0), "right": (1, 0)}.get(direction)
        if delta:
            self.move_cursor(*delta)

    def action_open_build(self) -> None:
        self.post_message(self.BuildRequested(self.cx, self.cy))

    def action_center_hq(self) -> None:
        """把光标移到指挥中心（第一座 hq 类型建筑）。"""
        s = self._snapshot
        if s is None:
            return
        for b in s.buildings:
            if b.alive and b.type_key == "hq":
                self.set_cursor(b.x, b.y)
                return

    # ---------------- 当前格信息 ----------------

    def _adjacent_count(self, x: int, y: int, terrain_char: int) -> int:
        """统计 8 邻域内某地形的格数（与引擎 adjIce/adjMountain 同为 8 邻域）。"""
        c, s = self._content, self._snapshot
        if c is None or s is None:
            return 0
        n = 0
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if dx == 0 and dy == 0:
                    continue
                t = s.tile(x + dx, y + dy, c.map_w)
                if t is not None and t.terrain == terrain_char:
                    n += 1
        return n

    def cell_description(self) -> Text:
        """当前光标格的文本描述（地形 / 矿脉 / 占用建筑 / 相邻加成，对齐旧 CLI 的 scan）。"""
        c, s = self._content, self._snapshot
        t = Text(no_wrap=True)
        if c is None or s is None:
            return Text("（载入中）", style="bright_black")
        x, y = self.cx, self.cy
        t.append(f"({x},{y}) ", style="bright_black")
        tile = s.tile(x, y, c.map_w)
        if tile is None:
            t.append("越界", style="red")
            return t
        t.append(terrain_glyph(tile.terrain), style=terrain_style(tile.terrain) or "white")
        t.append(" " + terrain_name(tile.terrain), style="white")
        if tile.terrain == ord("*"):
            t.append(f"  矿量 {tile.ore} 丰度 {tile.richness}", style="yellow")
        b = s.building(tile.building) if tile.building >= 0 else None
        if b is not None:
            info = c.building_by_index(b.type)
            name = info.name if info else b.type_key
            t.append(f"  [{b.type_key}] {name} #{b.id}", style="cyan")
            t.append(f"  {b.status_text}", style="yellow" if b.status_key != "ok" else "green")
            t.append(f"  工人 {s.assigned_for(b.id)}", style="bright_black")
        elif tile.building < 0:
            # 空地：给出选址加成（农场看邻冰、矿场看邻山），帮助玩家不用背手册
            adj_ice = self._adjacent_count(x, y, ord("|"))
            adj_mtn = self._adjacent_count(x, y, ord("^"))
            t.append(f"  邻冰 {adj_ice} 邻山 {adj_mtn}", style="bright_black")
        return t

    # ---------------- 渲染 ----------------

    def _ensure_visible(self, cols: int, rows: int) -> None:
        c = self._content
        if c is None:
            return
        if self.cx < self.off_x:
            self.off_x = self.cx
        if self.cx >= self.off_x + cols:
            self.off_x = self.cx - cols + 1
        if self.cy < self.off_y:
            self.off_y = self.cy
        if self.cy >= self.off_y + rows:
            self.off_y = self.cy - rows + 1
        self.off_x = max(0, min(self.off_x, max(0, c.map_w - cols)))
        self.off_y = max(0, min(self.off_y, max(0, c.map_h - rows)))

    def _cell(self, x: int, y: int) -> tuple[str, str]:
        """返回 (字形, 样式)。建筑字形/颜色来自 content_info，地形字形来自字符编码。"""
        c, s = self._content, self._snapshot
        assert c is not None and s is not None
        tile = s.tile(x, y, c.map_w)
        if tile is None:
            return (" ", "")
        if tile.building >= 0:
            b = s.building(tile.building)
            if b is not None:
                info = c.building_by_index(b.type)
                glyph = (info.glyph_upper if info else "?")
                base = col_style(info.color) if info else ""
                if not b.alive:
                    return (".", "bright_black")
                if b.build_left > 0:
                    return (glyph.lower(), "bright_black")
                if b.damaged > 0:
                    return ("!", "red")
                if not b.enabled:
                    return ("/", "bright_black")
                return (glyph, base or "white")
        return (terrain_glyph(tile.terrain), terrain_style(tile.terrain))

    def _legend(self) -> Text:
        """图例：只显示当前地图上真实出现过的地形（动态，非硬编码列表）。"""
        c, s = self._content, self._snapshot
        t = Text(no_wrap=True)
        if c is None or s is None:
            return t
        seen: list[int] = []
        for tile in s.tiles:
            if tile.terrain not in seen:
                seen.append(tile.terrain)
        t.append("图例 ", style="bright_black")
        for code in seen:
            t.append(terrain_glyph(code), style=terrain_style(code) or "white")
            t.append(f" {terrain_name(code)}  ", style="bright_black")
        return t

    def render(self) -> Text:
        c, s = self._content, self._snapshot
        if c is None or s is None or c.map_w <= 0 or c.map_h <= 0:
            return Text("正在载入地图…", style="bright_black")

        avail_w = max(8, self.size.width)
        avail_h = max(3, self.size.height)
        cols = max(1, (avail_w - GUTTER) // CELL_W)
        legend = avail_h >= 7
        grid_rows = max(1, avail_h - 2 - (1 if legend else 0))
        rows = min(c.map_h, grid_rows)
        self._ensure_visible(cols, rows)

        out = Text(no_wrap=True)

        # 标题行
        out.append("殖民地地图", style="bold blue")
        out.append("  ↑↓←→ 移动 · Enter 建造 · g 回指挥中心", style="bright_black")
        out.append("\n")

        # 列号
        out.append(" " * GUTTER, style="")
        for x in range(self.off_x, min(c.map_w, self.off_x + cols)):
            out.append(f"{x:>{CELL_W}}", style="bright_black")
        out.append("\n")

        # 网格
        max_x = min(c.map_w, self.off_x + cols)
        for y in range(self.off_y, min(c.map_h, self.off_y + rows)):
            out.append(f"{y:>2} ", style="bright_black")
            for x in range(self.off_x, max_x):
                glyph, style = self._cell(x, y)
                if x == self.cx and y == self.cy:
                    out.append(glyph.ljust(CELL_W), style=(style + " reverse") if style else "reverse")
                else:
                    out.append(glyph, style=style)
                    out.append(" " * (CELL_W - 1))
            out.append("\n")

        if legend:
            out.append_text(self._legend())
        return out
