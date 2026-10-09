"""资源栏：一屏看懂全局（资源 + 净收支、周期/人口/士气/防御/虫潮/天气）。"""

from __future__ import annotations

from typing import Optional

from rich.text import Text
from textual.widgets import Static

from ..i18n import col_style, net_style, sign
from ..models import ContentInfo, Snapshot


class ResourceBar(Static):
    """顶部两行状态栏。数据全部来自 snapshot / content_info。"""

    DEFAULT_CSS = """
    ResourceBar {
        height: 2;
        padding: 0 1;
        background: $panel;
    }
    """

    def __init__(self, **kwargs) -> None:
        kwargs.setdefault("markup", False)
        super().__init__("", **kwargs)
        self._snapshot: Optional[Snapshot] = None
        self._content: Optional[ContentInfo] = None

    def set_state(self, snapshot: Optional[Snapshot], content: Optional[ContentInfo]) -> None:
        """更新数据并重绘。"""
        self._snapshot = snapshot
        self._content = content
        self.update(self._render_text())

    # ---------------- 内部 ----------------

    def _render_text(self) -> Text:
        s = self._snapshot
        c = self._content
        if s is None or c is None:
            return Text("正在载入殖民地状态…（连接 starcolony-rpc）", style="bright_black")

        t = Text(no_wrap=True)

        # ---- 第 1 行：周期 + 四种资源（存量 + 净收支）----
        t.append("周期 ", style="bright_black")
        t.append(f"{s.turn}", style="bold white")
        t.append(f"/{c.max_turns}", style="bright_black")
        t.append("   ")
        t.append(f"[{s.colony_name}]", style="magenta")
        if s.seed is not None:
            # 新增字段：本局实际种子（字段缺失时整段不显示，避免猜一个值）
            t.append("  种子 ", style="bright_black")
            t.append(str(s.seed), style="bright_white")
        t.append("    ")

        for label, value, net, key in (
            ("金属", s.metal, s.metal_net, "bright_white"),
            ("能源", s.energy, s.energy_net, "yellow"),
            ("食物", s.food, s.food_net, "green"),
            ("科研", s.science, s.science_net, "cyan"),
        ):
            t.append(label + " ", style="bright_black")
            t.append(str(value), style=key)
            t.append(f"({sign(net)})", style=net_style(net))
            t.append("  ")

        t.append("\n")

        # ---- 第 2 行：人口/住房/士气/防御/虫潮/天气 ----
        over = s.overlarge
        t.append("人口 ", style="bright_black")
        t.append(f"{s.pop}/{s.housing}", style=("bold red" if over else "white"))
        if over:
            t.append(" 超编!", style="bold red")
        t.append(f"（闲置 {s.idle_workers}）", style="bright_black")
        t.append("   ")

        morale_style = "red" if s.morale < 40 else ("green" if s.morale >= 70 else "yellow")
        t.append("士气 ", style="bright_black")
        t.append(str(s.morale), style=morale_style)
        t.append("   ")

        # 防御力相对虫潮预估强度：偏低标红
        def_style = "green" if s.defense >= s.wave_strength_estimate else "red"
        t.append("防御 ", style="bright_black")
        t.append(str(s.defense), style=def_style)
        t.append("   ")

        wave_soon = s.wave_in <= 3
        t.append("下一波虫潮 ", style="bright_black")
        t.append(f"{s.wave_in} 周期", style=("bold red" if wave_soon else "magenta"))
        t.append(f"（预计强度 {s.wave_strength_estimate}）", style="bright_black")
        t.append("   ")

        w = c.weather(s.weather)
        if w is not None:
            wstyle = col_style(w.color, "white") or "white"
            t.append("天气 ", style="bright_black")
            t.append(f"{w.name}({s.weather_left})", style=f"bold {wstyle}")

        return t
