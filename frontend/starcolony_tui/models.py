"""协议 JSON 的类型化封装（防御式解析）。

对应 docs/PROTOCOL.md §4.2 ``content_info`` / §4.4 ``snapshot`` / §4.5 ``LogEntry``。

原则：
* 所有字段都用 ``.get`` 取值并给默认值 —— 契约 §8 说「新增字段向后兼容，
  前端应忽略不认识的字段」，反过来「字段缺失」也不能让前端崩溃；
* 不做任何规则判定，只做结构化承载与 O(1) 索引；
* 绝不硬编码 mapW/mapH、造价、地形表 —— 这些一律来自 ``content_info``。
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Iterable, Optional


def _as_int(value: Any, default: int = 0) -> int:
    """把 JSON 里的数安全转成 int（bool 视为非法，回退默认）。"""
    if isinstance(value, bool):
        return default
    if isinstance(value, int):
        return value
    if isinstance(value, float):
        return int(value)
    return default


def _maybe_int(value: Any) -> Optional[int]:
    """把 JSON 里的数转成 int；不是数（含缺失、bool、字符串）时返回 ``None``。

    与 :func:`_as_int` 的区别：``None`` 表示「服务端未提供该字段」，
    用来在界面层区分「真的为 0」与「字段不存在，需回退」。
    """
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, float):
        return int(value)
    return None


def _as_bool(value: Any, default: bool = False) -> bool:
    return value if isinstance(value, bool) else default


def _as_str(value: Any, default: str = "") -> str:
    return value if isinstance(value, str) else default


def _as_list(value: Any) -> list[Any]:
    return value if isinstance(value, list) else []


# ============================================================
#  content_info（§4.2）
# ============================================================

@dataclass(frozen=True)
class BuildingInfo:
    """一种建筑的定义（content_info.buildings[]，来自引擎的 BDEF）。"""

    key: str
    name: str
    glyph: str
    color: int
    cost_metal: int
    cost_energy: int
    cost_science: int
    build_turns: int
    workers: int
    upkeep: int
    repeatable: bool
    desc: str

    @classmethod
    def from_json(cls, obj: dict[str, Any]) -> "BuildingInfo":
        return cls(
            key=_as_str(obj.get("key")),
            name=_as_str(obj.get("name")),
            glyph=_as_str(obj.get("glyph"), "?"),
            color=_as_int(obj.get("color"), 0),
            cost_metal=_as_int(obj.get("costMetal")),
            cost_energy=_as_int(obj.get("costEnergy")),
            cost_science=_as_int(obj.get("costScience")),
            build_turns=_as_int(obj.get("buildTurns")),
            workers=_as_int(obj.get("workers")),
            upkeep=_as_int(obj.get("upkeep")),
            repeatable=_as_bool(obj.get("repeatable"), True),
            desc=_as_str(obj.get("desc")),
        )

    @property
    def glyph_upper(self) -> str:
        """地图/快捷键用的字形（大写，单字符）。"""
        return (self.glyph or "?")[:1].upper()


@dataclass(frozen=True)
class TechInfo:
    """一项科技的定义（content_info.techs[]，来自引擎的 TDEF）。"""

    key: str
    name: str
    cost: int
    req: tuple[str, ...]
    desc: str

    @classmethod
    def from_json(cls, obj: dict[str, Any]) -> "TechInfo":
        req = tuple(_as_str(r) for r in _as_list(obj.get("req")))
        return cls(
            key=_as_str(obj.get("key")),
            name=_as_str(obj.get("name")),
            cost=_as_int(obj.get("cost")),
            req=req,
            desc=_as_str(obj.get("desc")),
        )


@dataclass(frozen=True)
class WeatherInfo:
    """一种天气的定义（content_info.weathers[]，来自引擎的 WDEF）。"""

    name: str
    color: int
    metal: float
    energy: float
    food: float
    science: float
    desc: str

    @classmethod
    def from_json(cls, obj: dict[str, Any]) -> "WeatherInfo":
        def f(key: str) -> float:
            v = obj.get(key)
            return float(v) if isinstance(v, (int, float)) and not isinstance(v, bool) else 1.0

        return cls(
            name=_as_str(obj.get("name")),
            color=_as_int(obj.get("color")),
            metal=f("metal"),
            energy=f("energy"),
            food=f("food"),
            science=f("science"),
            desc=_as_str(obj.get("desc")),
        )


@dataclass
class ContentInfo:
    """引擎内容信息。启动时拉一次并缓存（§4.2）。"""

    map_w: int = 0
    map_h: int = 0
    max_turns: int = 0
    buildings: list[BuildingInfo] = field(default_factory=list)
    techs: list[TechInfo] = field(default_factory=list)
    weathers: list[WeatherInfo] = field(default_factory=list)
    _by_key: dict[str, BuildingInfo] = field(default_factory=dict, repr=False)
    _tech_by_key: dict[str, TechInfo] = field(default_factory=dict, repr=False)

    @classmethod
    def from_json(cls, obj: dict[str, Any]) -> "ContentInfo":
        buildings = [BuildingInfo.from_json(b) for b in _as_list(obj.get("buildings")) if isinstance(b, dict)]
        techs = [TechInfo.from_json(t) for t in _as_list(obj.get("techs")) if isinstance(t, dict)]
        weathers = [WeatherInfo.from_json(w) for w in _as_list(obj.get("weathers")) if isinstance(w, dict)]
        ci = cls(
            map_w=_as_int(obj.get("mapW")),
            map_h=_as_int(obj.get("mapH")),
            max_turns=_as_int(obj.get("maxTurns")),
            buildings=buildings,
            techs=techs,
            weathers=weathers,
        )
        ci._by_key = {b.key: b for b in buildings}
        ci._tech_by_key = {t.key: t for t in techs}
        return ci

    # ---- 查询辅助 ----
    def building(self, key: str) -> Optional[BuildingInfo]:
        """按键取建筑定义；未知键返回 ``None``。"""
        return self._by_key.get(key)

    def building_by_index(self, index: int) -> Optional[BuildingInfo]:
        """按 ``BuildingView.type``（BDEF 下标）取定义。"""
        if 0 <= index < len(self.buildings):
            return self.buildings[index]
        return None

    def tech(self, key: str) -> Optional[TechInfo]:
        return self._tech_by_key.get(key)

    def weather(self, index: int) -> Optional[WeatherInfo]:
        if 0 <= index < len(self.weathers):
            return self.weathers[index]
        return None

    def building_index(self, key: str) -> int:
        """建筑 key -> BDEF 下标；未知返回 -1。"""
        for i, b in enumerate(self.buildings):
            if b.key == key:
                return i
        return -1

    def resolve_building_key(self, token: str) -> Optional[BuildingInfo]:
        """按 key 或中文名解析建筑（与引擎 doBuild 的匹配口径一致）。"""
        if not token:
            return None
        if token in self._by_key:
            return self._by_key[token]
        for b in self.buildings:
            if b.name == token:
                return b
        return None


# ============================================================
#  snapshot（§4.4）
# ============================================================

@dataclass(frozen=True)
class Tile:
    """一个地块（tiles[]，按 y*mapW+x 展平）。"""

    terrain: int
    ore: int
    richness: int
    building: int

    @classmethod
    def from_json(cls, obj: dict[str, Any]) -> "Tile":
        return cls(
            terrain=_as_int(obj.get("terrain")),
            ore=_as_int(obj.get("ore")),
            richness=_as_int(obj.get("richness")),
            building=_as_int(obj.get("building"), -1),
        )


@dataclass(frozen=True)
class BuildingState:
    """一座已存在的建筑（snapshot.buildings[]）。

    ``worker_need`` 来自新增字段 ``buildings[].workerNeed``（已计入科技修正）。
    字段缺失时保持 ``None``，界面回退到「只显示已分配工人」的旧行为。
    """

    id: int
    type: int
    type_key: str
    x: int
    y: int
    build_left: int
    damaged: int
    enabled: bool
    alive: bool
    assigned: int
    worker_need: Optional[int] = None

    @classmethod
    def from_json(cls, obj: dict[str, Any]) -> "BuildingState":
        raw_need = obj.get("workerNeed")
        need = raw_need if isinstance(raw_need, int) and not isinstance(raw_need, bool) else None
        return cls(
            id=_as_int(obj.get("id"), -1),
            type=_as_int(obj.get("type")),
            type_key=_as_str(obj.get("typeKey")),
            x=_as_int(obj.get("x")),
            y=_as_int(obj.get("y")),
            build_left=_as_int(obj.get("buildLeft")),
            damaged=_as_int(obj.get("damaged")),
            enabled=_as_bool(obj.get("enabled"), True),
            alive=_as_bool(obj.get("alive"), True),
            assigned=_as_int(obj.get("assigned")),
            worker_need=need,
        )

    @property
    def has_worker_need(self) -> bool:
        """服务端是否提供了真实工人需求（区别于前端回退）。"""
        return self.worker_need is not None

    @property
    def is_understaffed(self) -> bool:
        """是否人手不足（仅在拿到真实 ``workerNeed`` 时才有意义）。"""
        return (
            self.worker_need is not None
            and self.worker_need > 0
            and self.status_key == "ok"
            and self.assigned < self.worker_need
        )

    @property
    def status_key(self) -> str:
        """状态枚举名：``building`` / ``damaged`` / ``off`` / ``short`` / ``ok``。"""
        if not self.alive:
            return "dead"
        if self.build_left > 0:
            return "building"
        if self.damaged > 0:
            return "damaged"
        if not self.enabled:
            return "off"
        return "ok"

    @property
    def status_text(self) -> str:
        if not self.alive:
            return "已拆除"
        if self.build_left > 0:
            return f"在建 剩 {self.build_left}"
        if self.damaged > 0:
            return f"受损 剩 {self.damaged}"
        if not self.enabled:
            return "已关闭"
        return "正常"


@dataclass(frozen=True)
class PendingEvent:
    """待决事件（§4.4.1）。``options`` 下标 0 对应 ``answer option=1``。"""

    kind: str
    title: str
    text: str
    options: tuple[str, ...] = ()

    @classmethod
    def from_json(cls, obj: Any) -> Optional["PendingEvent"]:
        if not isinstance(obj, dict):
            return None
        opts = tuple(_as_str(o) for o in _as_list(obj.get("options")))
        return cls(
            kind=_as_str(obj.get("kind")),
            title=_as_str(obj.get("title")),
            text=_as_str(obj.get("text")),
            options=opts,
        )


@dataclass(frozen=True)
class LogEntry:
    """一条结构化日志（§4.5）。``code`` 决定配色，``text`` 是服务端渲染好的中文。"""

    turn: int
    code: str
    ints: tuple[int, ...]
    strings: tuple[str, ...]
    text: str

    @classmethod
    def from_json(cls, obj: dict[str, Any]) -> "LogEntry":
        return cls(
            turn=_as_int(obj.get("turn")),
            code=_as_str(obj.get("code"), "Text"),
            ints=tuple(_as_int(v) for v in _as_list(obj.get("ints"))),
            strings=tuple(_as_str(s) for s in _as_list(obj.get("strings"))),
            text=_as_str(obj.get("text")),
        )


@dataclass(frozen=True)
class BuildPreview:
    """``preview_build`` 的返回（只读查询：某建筑在某地块能否建造、是否付得起）。

    ``buildable``（规则层：地形/占用/前置）与 ``affordable``（资源层）**相互独立**，
    界面据此分别标注「地形不符」与「资源不足」两种不同的灰显。
    服务端未实现该方法时前端拿到 ``None``，退回「不灰显」。
    """

    ok: bool
    buildable: Optional[bool]
    reason: str
    cost: dict[str, int]
    affordable: dict[str, bool]

    @classmethod
    def from_json(cls, obj: dict[str, Any]) -> "BuildPreview":
        raw_cost = obj.get("cost") if isinstance(obj.get("cost"), dict) else {}
        raw_aff = obj.get("affordable") if isinstance(obj.get("affordable"), dict) else {}
        buildable_raw = obj.get("buildable")
        return cls(
            ok=_as_bool(obj.get("ok")),
            buildable=buildable_raw if isinstance(buildable_raw, bool) else None,
            reason=_as_str(obj.get("reason")),
            cost={str(k): _as_int(v) for k, v in raw_cost.items()},
            affordable={str(k): _as_bool(v) for k, v in raw_aff.items()},
        )

    @property
    def affordable_all(self) -> bool:
        """是否所有资源都够（``affordable`` 缺失时视为够，避免误灰显）。"""
        if not self.affordable:
            return True
        return all(self.affordable.values())

    @property
    def lacking(self) -> list[str]:
        """缺哪些资源（中文名，供界面显示）。"""
        names = {"metal": "金属", "energy": "能源", "science": "科研"}
        return [names.get(k, k) for k, v in self.affordable.items() if not v]


@dataclass
class Snapshot:
    """一局游戏的完整状态快照（§4.4）。"""

    turn: int = 1
    metal: int = 0
    energy: int = 0
    food: int = 0
    science: int = 0
    pop: int = 0
    housing: int = 0
    morale: int = 0
    weather: int = 0
    weather_left: int = 0
    wave_in: int = 0
    wave_strength_estimate: int = 0
    defense: int = 0
    metal_in: int = 0
    energy_in: int = 0
    food_in: int = 0
    science_in: int = 0
    energy_up: int = 0
    food_up: int = 0
    metal_net: int = 0
    energy_net: int = 0
    food_net: int = 0
    science_net: int = 0
    brownout: bool = False
    starving: bool = False
    over: bool = False
    won: bool = False
    end_reason: str = ""
    colony_name: str = ""
    buildings: list[BuildingState] = field(default_factory=list)
    tiles: list[Tile] = field(default_factory=list)
    log: list[LogEntry] = field(default_factory=list)
    techs: list[str] = field(default_factory=list)
    assigned: list[int] = field(default_factory=list)
    pending: Optional[PendingEvent] = None
    # 新增可选字段：缺失时为 None，界面回退到旧行为
    idle_workers_reported: Optional[int] = None
    seed: Optional[int] = None
    # O(1) 索引（非协议字段）
    _by_id: dict[int, BuildingState] = field(default_factory=dict, repr=False)
    _tech_set: set[str] = field(default_factory=set, repr=False)

    @classmethod
    def from_json(cls, obj: dict[str, Any]) -> "Snapshot":
        snap = cls(
            turn=_as_int(obj.get("turn"), 1),
            metal=_as_int(obj.get("metal")),
            energy=_as_int(obj.get("energy")),
            food=_as_int(obj.get("food")),
            science=_as_int(obj.get("science")),
            pop=_as_int(obj.get("pop")),
            housing=_as_int(obj.get("housing")),
            morale=_as_int(obj.get("morale")),
            weather=_as_int(obj.get("weather")),
            weather_left=_as_int(obj.get("weatherLeft")),
            wave_in=_as_int(obj.get("waveIn")),
            wave_strength_estimate=_as_int(obj.get("waveStrengthEstimate")),
            defense=_as_int(obj.get("defense")),
            metal_in=_as_int(obj.get("metalIn")),
            energy_in=_as_int(obj.get("energyIn")),
            food_in=_as_int(obj.get("foodIn")),
            science_in=_as_int(obj.get("scienceIn")),
            energy_up=_as_int(obj.get("energyUp")),
            food_up=_as_int(obj.get("foodUp")),
            metal_net=_as_int(obj.get("metalNet")),
            energy_net=_as_int(obj.get("energyNet")),
            food_net=_as_int(obj.get("foodNet")),
            science_net=_as_int(obj.get("scienceNet")),
            brownout=_as_bool(obj.get("brownout")),
            starving=_as_bool(obj.get("starving")),
            over=_as_bool(obj.get("over")),
            won=_as_bool(obj.get("won")),
            end_reason=_as_str(obj.get("endReason")),
            colony_name=_as_str(obj.get("colonyName")),
            buildings=[BuildingState.from_json(b) for b in _as_list(obj.get("buildings")) if isinstance(b, dict)],
            tiles=[Tile.from_json(t) for t in _as_list(obj.get("tiles")) if isinstance(t, dict)],
            log=[LogEntry.from_json(e) for e in _as_list(obj.get("log")) if isinstance(e, dict)],
            techs=[_as_str(t) for t in _as_list(obj.get("techs"))],
            assigned=[_as_int(a) for a in _as_list(obj.get("assigned"))],
            pending=PendingEvent.from_json(obj.get("pending")),
            idle_workers_reported=_maybe_int(obj.get("idleWorkers")),
            seed=_maybe_int(obj.get("seed")),
        )
        snap._by_id = {b.id: b for b in snap.buildings}
        snap._tech_set = set(snap.techs)
        return snap

    # ---- 查询辅助 ----

    def building(self, building_id: int) -> Optional[BuildingState]:
        return self._by_id.get(building_id)

    def tile(self, x: int, y: int, map_w: int) -> Optional[Tile]:
        """按 (x,y) 取地块；越界或 map 未知返回 ``None``（前端不做规则判定）。"""
        if map_w <= 0 or x < 0 or y < 0:
            return None
        idx = y * map_w + x
        if 0 <= idx < len(self.tiles):
            return self.tiles[idx]
        return None

    def has_tech(self, key: str) -> bool:
        return key in self._tech_set

    @property
    def alive_buildings(self) -> list[BuildingState]:
        return [b for b in self.buildings if b.alive]

    @property
    def overlarge(self) -> bool:
        """人口是否超编（人口 > 住房）。"""
        return self.pop > self.housing

    def assigned_for(self, building_id: int) -> int:
        """该建筑的已分配工人数（优先用 snapshot.assigned，回退 BuildingState.assigned）。"""
        if 0 <= building_id < len(self.assigned):
            return self.assigned[building_id]
        b = self.building(building_id)
        return b.assigned if b else 0

    @property
    def estimated_idle_workers(self) -> int:
        """前端估算的闲置人口（回退用，非规则源）。

        仅在服务端未提供 ``idleWorkers`` 字段时使用；算法为
        ``人口 - Σ(运转中建筑的已分配工人)``，与引擎口径可能有细微差异。
        """
        used = 0
        for b in self.buildings:
            if b.build_left > 0 or not b.enabled or b.damaged > 0 or not b.alive:
                continue
            used += self.assigned_for(b.id)
        return max(0, self.pop - used)

    @property
    def idle_workers(self) -> int:
        """闲置人口：优先用服务端 ``idleWorkers``，缺失时回退到前端估算。"""
        if self.idle_workers_reported is not None:
            return self.idle_workers_reported
        return self.estimated_idle_workers

    @property
    def has_idle_workers_field(self) -> bool:
        return self.idle_workers_reported is not None

    def log_codes(self, entries: Optional[Iterable[LogEntry]] = None) -> list[str]:
        """取日志的 code 序列（便于测试与待决事件推断）。"""
        src = self.log if entries is None else entries
        return [e.code for e in src]


__all__ = [
    "BuildingInfo",
    "TechInfo",
    "WeatherInfo",
    "ContentInfo",
    "Tile",
    "BuildingState",
    "PendingEvent",
    "BuildPreview",
    "LogEntry",
    "Snapshot",
]
