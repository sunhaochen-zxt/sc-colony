"""``ActCode`` -> 配色 / 图标映射，以及地形与事件的显示辅助。

P2 策略（见 docs/PROTOCOL.md §4.5）：

* 前端**优先用 ``code`` 做布局与配色决策**（虫潮红、建造完成绿……）；
* 显示文本先用服务端渲染好的 ``text`` 字段，保证与旧 CLI 逐字一致。

本模块因此**只**负责「code -> 样式/图标」这类与语言无关的映射，不复制任何引擎文案。
颜色沿用引擎的 ``Col`` 整数枚举（``src/types.hpp:25-36``），mapW/mapH、建筑字形与造价
一律从 ``content_info`` 取，本文件不做任何数值/尺寸硬编码。

P3（内容外置）时，把 ``CODE_META`` 的 severity 留着，文案再迁到 ``content/i18n/zh-CN.json``。
"""

from __future__ import annotations

from typing import Optional

# ============================================================
#  Col 整数枚举 -> Rich / Textual 样式名
# ============================================================
# src/types.hpp：0 默认 1 灰 2 红 3 绿 4 黄 5 蓝 6 品红 7 青 8 白 9 亮白。
# 只映射成终端颜色名，绝不自己另编一套配色。
COL_STYLE: dict[int, str] = {
    0: "",               # COL_DEF 默认
    1: "bright_black",   # COL_GREY
    2: "red",
    3: "green",
    4: "yellow",
    5: "blue",
    6: "magenta",
    7: "cyan",
    8: "white",
    9: "bright_white",
}


def col_style(idx: object, default: str = "") -> str:
    """把引擎的 ``Col`` 整数映射成 Rich 样式名；未知/非法值回退 ``default``。"""
    try:
        return COL_STYLE.get(int(idx), default)  # type: ignore[arg-type]
    except (TypeError, ValueError):
        return default


# ============================================================
#  地形（显示偏好）
# ============================================================
# 协议把地形以「字符编码」下发（tiles[].terrain：'.'=46 '*'=42 '~'=126 '|'=124 '^'=94，
# 见 docs/PROTOCOL.md §4.4）。字形直接 ``chr(code)`` 渲染，**不做字形表硬编码**；
# 下面两张表只是「颜色/名字的显示偏好」，用于 scan/detail 面板的可读性，未知码有安全回退。
#
# 注意：content_info 目前**不提供**地形表（P3 内容外置时应补上），所以这里的名字是
# 前端兜底；它是纯展示信息，不参与任何规则判定。
TERRAIN_NAME: dict[int, str] = {
    46: "平原",      # '.'
    42: "金属矿脉",  # '*'
    126: "地热口",   # '~'
    124: "冰层",     # '|'
    94: "山脉",      # '^'
}

TERRAIN_STYLE: dict[int, str] = {
    46: "bright_black",  # 平原
    42: "yellow",        # 金属矿脉
    126: "magenta",      # 地热口
    124: "cyan",         # 冰层
    94: "bright_black",  # 山脉
}


def terrain_name(code: object) -> str:
    """地形显示名；未知字符编码给出可读的兜底，不崩溃。"""
    try:
        c = int(code)  # type: ignore[arg-type]
    except (TypeError, ValueError):
        return "未知地形"
    return TERRAIN_NAME.get(c, f"未知地形 0x{c:02X}")


def terrain_style(code: object) -> str:
    """地形配色（Rich 样式名）；未知码回退默认色。"""
    try:
        return TERRAIN_STYLE.get(int(code), "")  # type: ignore[arg-type]
    except (TypeError, ValueError):
        return ""


def terrain_glyph(code: object) -> str:
    """地形字形：直接由引擎下发的字符编码还原（不硬编码字形表）。"""
    try:
        c = int(code)  # type: ignore[arg-type]
        return chr(c) if 0 < c < 0x110000 else "?"
    except (TypeError, ValueError):
        return "?"


# ============================================================
#  ActCode -> 严重度 / 图标
# ============================================================
# 严重度只用于「按 code 上色 + 是否高亮」，与文案无关（文案用服务端 text）。
# 覆盖 src/protocol.hpp:33-105 的完整 ActCode 清单；未知 code 回退 normal。
#
# severity 语义：
#   danger 危险（红，加粗）      warn 警告（黄）
#   good   正面（绿，加粗）      trade 交易/收益（青）
#   info   信息（蓝）            struct 结构分隔（暗灰）
#   normal 兜底（默认色）

SEVERITY_STYLE: dict[str, str] = {
    "danger": "bold red",
    "warn": "yellow",
    "good": "bold green",
    "trade": "cyan",
    "info": "blue",
    "struct": "bright_black",
    "normal": "",
}

# 图标统一选单宽度字符，避免与引擎 text 里的图标叠加时错位。
CODE_META: dict[str, tuple[str, str]] = {
    # ---- 行动结果（ActionResult）----
    "Ok":                ("normal", ""),
    "BuildUnknownType":  ("warn", "✖"),
    "BuildBlocked":      ("warn", "✖"),
    "BuildStarted":      ("good", "✔"),
    "DemolishInvalid":   ("warn", "✖"),
    "DemolishHQ":        ("warn", "✖"),
    "DemolishDone":      ("info", "·"),
    "ToggleInvalid":     ("warn", "✖"),
    "ToggleDone":        ("info", "·"),
    "FocusInvalid":      ("warn", "✖"),
    "FocusDone":         ("info", "·"),
    "ResearchUnknown":   ("warn", "✖"),
    "ResearchDup":       ("warn", "✖"),
    "ResearchPrereq":    ("warn", "✖"),
    "ResearchNoScience": ("warn", "✖"),
    "ResearchDone":      ("good", "★"),
    "AnswerNone":        ("warn", "⚠"),
    "AnswerInvalid":     ("warn", "✖"),
    "AnswerChoice":      ("info", "▶"),
    "BlockedByPending":  ("warn", "⚠"),  # §4.4.1：待决事件期间非 answer 的 action 被引擎拒绝
    # ---- 结构化日志（LogEntry）----
    "Text":               ("normal", "·"),
    "TurnHeader":         ("struct", "─"),
    "PopBorn":            ("good", "＋"),
    "WeatherChange":      ("info", "☁"),
    "EventRefugees":      ("info", "◇"),
    "EventMarket":        ("info", "◇"),
    "EventSignal":        ("info", "◇"),
    "EventLifeSupport":   ("info", "◇"),
    "EventMeteorHit":     ("warn", "☄"),
    "EventMeteorMiss":    ("normal", "☄"),
    "EventProspectFound": ("good", "◈"),
    "EventProspectNone":  ("normal", "◈"),
    "EventVentFound":     ("good", "◈"),
    "EventVentNone":      ("normal", "◈"),
    "EventFestival":      ("good", "♪"),
    "EventCaravan":       ("trade", "⛟"),
    "LogChoice":          ("info", "▶"),
    "RefugeesSettled":    ("good", "＋"),
    "OvercrowdWarn":      ("warn", "⚠"),
    "RefugeesPartial":    ("warn", "⚠"),
    "RefugeesRefused":    ("warn", "⚠"),
    "RefugeesSeized":     ("trade", "⛟"),
    "MarketNoMetal":      ("warn", "⚠"),
    "MarketTradeMetal":   ("trade", "⇄"),
    "MarketNoEnergy":     ("warn", "⚠"),
    "MarketTradeEnergy":  ("trade", "⇄"),
    "MarketLeave":        ("good", "·"),
    "SignalSuccess":      ("good", "◈"),
    "SignalSwarm":        ("danger", "☣"),
    "SignalIgnore":       ("normal", "·"),
    "LifeSupportFixed":   ("good", "✔"),
    "LifeSupportPoor":    ("warn", "⚠"),
    "LifeSupportFail":    ("danger", "✖"),
    "WaveIncoming":       ("danger", "☣"),
    "WaveRepelled":       ("good", "✔"),
    "BuildingDamaged":    ("warn", "✖"),
    "WaveBreached":       ("danger", "✖"),
    "Repaired":           ("good", "✔"),
    "Brownout":           ("warn", "⚠"),
    "Starve":             ("danger", "⚠"),
    "OreDepleted":        ("warn", "◇"),
    "BuildingBuilt":      ("good", "✔"),
    "OvercrowdLeft":      ("warn", "⚠"),
    "AcidRain":           ("warn", "☂"),
    "GateTheoryUnlocked": ("good", "★"),
    "NewGameText":        ("info", "·"),
    "GoalText":           ("info", "·"),
    "HintText":           ("info", "·"),
    "GameOver":           ("danger", "×"),
}


def severity_for_code(code: object) -> str:
    """返回 ActCode 的严重度标签；未知 code 回退 ``normal``（向后兼容，见 PROTOCOL §8）。"""
    meta = CODE_META.get(str(code))
    return meta[0] if meta else "normal"


def style_for_code(code: object) -> str:
    """返回 ActCode 对应的 Rich 样式名。"""
    return SEVERITY_STYLE.get(severity_for_code(code), "")


def icon_for_code(code: object) -> str:
    """返回 ActCode 对应的单宽图标；未知 code 返回空串。"""
    meta = CODE_META.get(str(code))
    return meta[1] if meta else ""


def is_highlight(code: object) -> bool:
    """该 code 是否需要在日志里加粗高亮（危险/正面）。"""
    return severity_for_code(code) in ("danger", "good")


# ============================================================
#  待决事件（兜底表）
# ============================================================
#  正式数据来源是契约 §4.4.1 的 ``snapshot.pending``（含 kind/title/text/options），
#  前端应优先用它渲染事件弹窗（见 app.py 的 ``pending_event``）。
#
#  下面这张表**只是兼容兜底**：仅当连接到一个尚未实现 §4.4.1、snapshot.pending 恒为
#  null 的旧服务端时，前端才退化为「从日志公告码推断 + 用本表重建文案」。内容取自
#  docs/MANUAL.md §10。新服务端下本表不会被用到。
BLOCKING_EVENTS: frozenset[str] = frozenset(
    {"EventRefugees", "EventMarket", "EventSignal", "EventLifeSupport"}
)

EventDef = dict[str, object]

EVENT_TABLE: dict[str, EventDef] = {
    "EventRefugees": {
        "icon": "◇",
        "title": "难民船请求降落",
        "text": "一艘破旧的运输船在轨道上请求降落。接收他们会消耗食物、增加人口，"
                "并可能造成超编（超编每周期扣士气、可能流失人口）。",
        "options": [
            "接收难民（消耗食物，人口增加）",
            "拒绝降落（士气 -5）",
            "征用他们的补给（金属 +60，士气 -10）",
        ],
    },
    "EventMarket": {
        "icon": "◇",
        "title": "黑市商人",
        "text": "一个自称「自由商人」的家伙愿意和你做点交易。",
        "options": [
            "用 80 金属换 90 科研点",
            "用 120 能源换 220 金属",
            "礼貌送客（士气 +2）",
        ],
    },
    "EventSignal": {
        "icon": "◇",
        "title": "神秘信号",
        "text": "深空传来一段规律信号，似乎来自行星背面的遗迹。",
        "options": [
            "派队调查（50% 获得大量科研，50% 惊动虫群）",
            "忽略它（士气 +1）",
        ],
    },
    "EventLifeSupport": {
        "icon": "◇",
        "title": "维生系统故障",
        "text": "居住区的空气循环系统出现故障，修复需要 60 金属。",
        "options": [
            "花 60 金属紧急修复（金属不足时 -1 人口、士气 -3）",
            "先凑合着用（-3 人口，士气 -6）",
        ],
    },
}


def event_definition(code: object) -> Optional[EventDef]:
    """查待决事件的兜底定义；未知事件返回 ``None``（调用方走通用兜底）。"""
    return EVENT_TABLE.get(str(code))


# ============================================================
#  杂项
# ============================================================

def sign(value: int) -> str:
    """给资源净收支加正负号，例：``-6`` -> ``-6``、``2`` -> ``+2``。"""
    return f"+{value}" if value >= 0 else str(value)


def net_style(value: int) -> str:
    """净收支配色：正绿、负红、零灰。"""
    if value > 0:
        return "green"
    if value < 0:
        return "red"
    return "bright_black"


def bar_style(value: int, warn_below: int = 0) -> str:
    """资源存量配色：低于警戒线标红，否则默认。"""
    return "red" if value < warn_below else ""


__all__ = [
    "COL_STYLE",
    "col_style",
    "TERRAIN_NAME",
    "TERRAIN_STYLE",
    "terrain_name",
    "terrain_style",
    "terrain_glyph",
    "SEVERITY_STYLE",
    "CODE_META",
    "severity_for_code",
    "style_for_code",
    "icon_for_code",
    "is_highlight",
    "BLOCKING_EVENTS",
    "EVENT_TABLE",
    "event_definition",
    "sign",
    "net_style",
    "bar_style",
]
