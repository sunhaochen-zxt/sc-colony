"""帮助：快捷键总表 + 从 ``content_info`` 生成的建筑/科技/天气速查。

目的：**让玩家不用背手册也能玩**。所有数值都从 content_info 取，绝不硬编码。
"""

from __future__ import annotations

from typing import Optional

from rich.text import Text
from textual.binding import Binding
from textual.containers import VerticalScroll
from textual.screen import ModalScreen
from textual.widgets import Static

from ..i18n import col_style, TERRAIN_NAME
from ..models import ContentInfo

KEYMAP: list[tuple[str, str]] = [
    ("方向键 / ↑↓←→", "移动地图光标（在看建筑/科技列表时用于选择）"),
    ("Enter", "在地图光标处打开建造菜单（也可用建筑快捷键直接建造）"),
    ("Space / n", "推进一个周期（结算生产、人口、事件、虫潮）"),
    ("建筑快捷键 C S G M F H L K T X", "直接在当前光标格建造该建筑（取自建筑字形）"),
    ("d / x", "拆除选中建筑（返还一半金属；指挥中心不可拆）"),
    ("t", "开 / 关选中建筑（关闭后不耗能也不产出）"),
    ("f", "把选中建筑设为工人分配最优先"),
    ("r", "研究选中的科技"),
    ("e", "打开待决事件（有待决事件时推进周期会被拦住）"),
    ("g", "把地图光标移到指挥中心"),
    ("F2 / F3 / F4 / F5", "切换右侧面板：建筑 / 科技 / 日志 / 帮助"),
    ("s / l", "存档 / 读档（弹出路径输入）"),
    ("Ctrl+n", "以新随机种子重开一局"),
    ("? ", "打开本帮助"),
    ("q / Ctrl+q", "退出（自动回收服务端子进程）"),
]


def _sep(title: str) -> Text:
    t = Text()
    t.append("── ", style="bright_black")
    t.append(title, style="bold blue")
    t.append(" " + "─" * 40, style="bright_black")
    return t


def help_text(content: Optional[ContentInfo]) -> Text:
    """生成完整帮助文本（Text，带配色）。"""
    t = Text()
    t.append_text(_sep("目标与胜负"))
    t.append("\n")
    t.append("胜利：先研究「星门理论」，再建成星门，即撤离成功。\n", style="green")
    t.append("失败：人口归零，或撑过周期上限仍未建成星门。\n", style="red")
    c = content
    if c is not None:
        t.append(f"本局周期上限：{c.max_turns}    地图：{c.map_w}×{c.map_h}\n", style="bright_black")
    t.append("\n")

    t.append_text(_sep("快捷键"))
    t.append("\n")
    for keys, desc in KEYMAP:
        t.append(f"  {keys:<28}", style="bold yellow")
        t.append(desc, style="white")
        t.append("\n")
    t.append("\n")

    if c is not None:
        t.append_text(_sep("建筑表（字形 / 键 / 名称 / 造价 / 工期 / 工人 / 耗能）"))
        t.append("\n")
        t.append("  字形 键     名称        金属  能源  科研  工期  工人  耗能  可重复\n", style="bright_black")
        for b in c.buildings:
            t.append(f"   [{b.glyph_upper}] ", style=col_style(b.color) or "white")
            t.append(f"{b.key:<6}", style="bold white")
            t.append(f"{b.name:<10}", style=(col_style(b.color) or "white"))
            t.append(f"{b.cost_metal:>5}{b.cost_energy:>6}{b.cost_science:>6}{b.build_turns:>6}"
                     f"{b.workers:>6}{b.upkeep:>6}", style="bright_white")
            t.append(("   是" if b.repeatable else "   否"), style="bright_black")
            t.append("\n")
            t.append("        " + b.desc, style="bright_black")
            t.append("\n")
        t.append("\n")

        t.append_text(_sep("科技树（键 / 名称 / 科研 / 前置 / 效果）"))
        t.append("\n")
        for tech in c.techs:
            t.append(f"   {tech.key:<11}", style="bold cyan")
            t.append(f"{tech.name:<12}", style="white")
            t.append(f"科研 {tech.cost:<5}", style="cyan")
            if tech.req:
                names = [c.tech(r).name if c.tech(r) else r for r in tech.req]
                t.append("前置：" + " ".join(names) + "  ", style="bright_black")
            t.append("\n")
            t.append("        " + tech.desc, style="bright_black")
            t.append("\n")
        t.append("\n")

        t.append_text(_sep("天气修正（产出倍率）"))
        t.append("\n")
        for w in c.weathers:
            t.append(f"   {w.name:<6}", style=(col_style(w.color) or "white"))
            t.append(f"金属×{w.metal:.2f}  能源×{w.energy:.2f}  食物×{w.food:.2f}  科研×{w.science:.2f}  ",
                     style="bright_white")
            t.append(w.desc, style="bright_black")
            t.append("\n")
        t.append("\n")

    t.append_text(_sep("地形图例"))
    t.append("\n")
    t.append("  ")
    for code, name in TERRAIN_NAME.items():
        t.append(chr(code) + " " + name + "   ", style="bright_white")
    t.append("\n")
    t.append("  建造合法性由引擎在建造时校验：建不了会在下方提示具体原因（地形不符 / 已有建筑 / 资源不足）。\n",
             style="bright_black")
    t.append("\n")
    t.append_text(_sep("小贴士"))
    t.append("\n")
    t.append("  · 农场建在相邻冰层最多的平原/冰层上（每格相邻冰层 +2 食物，最多 +6）。\n", style="bright_black")
    t.append("  · 矿场建在丰度高、相邻山脉多的矿脉上（每格相邻山脉 +2 金属，最多 +6；矿脉会枯竭）。\n",
             style="bright_black")
    t.append("  · 地图光标下的状态行会显示当前格的「邻冰 / 邻山」数量，据此选址。\n", style="bright_black")
    t.append("  · 日志按事件类别着色：红色危险、绿色正面、黄色警告、青色交易。\n", style="bright_black")
    return t


class HelpPanel(VerticalScroll):
    """帮助面板（右侧「帮助」页签）。"""

    DEFAULT_CSS = """
    HelpPanel { height: 1fr; padding: 0 1; }
    """

    def compose(self):
        yield Static(help_text(None), id="help-body")

    def set_content(self, content: Optional[ContentInfo]) -> None:
        self.query_one("#help-body", Static).update(help_text(content))


class HelpScreen(ModalScreen[None]):
    """帮助弹窗（``?``）。"""

    BINDINGS = [Binding("escape", "dismiss_help", "关闭"),
                Binding("question_mark", "dismiss_help", "关闭", show=False)]

    DEFAULT_CSS = """
    HelpScreen { align: center middle; }
    HelpScreen > VerticalScroll {
        width: 90%; height: 88%; border: round $accent; background: $surface; padding: 1 2;
    }
    """

    def __init__(self, content: Optional[ContentInfo] = None) -> None:
        super().__init__()
        self._content = content

    def compose(self):
        with VerticalScroll():
            yield Static(help_text(self._content), id="help-modal-body")

    def action_dismiss_help(self) -> None:
        self.dismiss(None)
