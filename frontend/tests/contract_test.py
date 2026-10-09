#!/usr/bin/env python3
# 星际争霸：殖民地 (Star Colony) —— P2 JSON-RPC 契约测试（独立验收）
# 作者：QA（严过关 / Edward），独立实现，不复用工程师的任何自测断言。
#
# 唯一验收依据：docs/PROTOCOL.md（v1 冻结版）。
#
# 本文件包含两层：
#   1) 可复用客户端封装 RpcClient（子进程管理 + 行协议编解码 + 超时保护 + 退出清理）；
#   2) 基于它的契约测试（A–I 组，含对抗性协议健壮性用例）。
#
# 运行：
#   export TMPDIR=/home/shc/starcolony/build/tmp
#   .venv/bin/python -m pytest frontend/tests/contract_test.py -v
#
# 用 mock 自验 harness（不依赖真实服务端）：
#   STARCOLONY_RPC_BIN=build/qa_mock/mock_rpc.py \
#   .venv/bin/python -m pytest frontend/tests/contract_test.py -k "not I"
#
# 环境变量：
#   STARCOLONY_RPC_BIN    覆盖被测服务端路径（.py 会用当前解释器启动）
#   STARCOLONY_RPC_ARGS   追加的启动参数（shlex 切分）
#   CONTRACT_TIMEOUT      单次响应超时秒数（默认 15）
#   CONTRACT_ORDER_N      顺序性用例的请求条数（默认 1000）
from __future__ import annotations

import json
import os
import queue
import re
import shlex
import subprocess
import sys
import threading
import uuid
from pathlib import Path

import pytest

# ---------------------------------------------------------------------------
#  路径与环境
# ---------------------------------------------------------------------------
ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build"
SRC = ROOT / "src"
DEFAULT_RPC = BUILD / "starcolony-rpc"
CLI_BIN = BUILD / "starcolony"
BASELINE_CLI = BUILD / "qa_tmp" / "starcolony_baseline"
TMP = BUILD / "qa_tmp"
STDERR_DIR = TMP / "stderr"

RESP_TIMEOUT = float(os.environ.get("CONTRACT_TIMEOUT", "15"))
ORDER_N = int(os.environ.get("CONTRACT_ORDER_N", "1000"))

# 沙箱 /tmp 不可靠：把所有临时产物锁死在 build/ 下
for _d in (BUILD / "tmp", TMP, STDERR_DIR):
    _d.mkdir(parents=True, exist_ok=True)
os.environ.setdefault("TMPDIR", str(BUILD / "tmp"))


# ---------------------------------------------------------------------------
#  可复用客户端封装
# ---------------------------------------------------------------------------
class RpcError(Exception):
    """harness 自身或传输层错误（非被测语义）。"""


class RpcClient:
    """把服务端当子进程管理，按「一行一个 JSON 对象」读写，带超时保护。

    读取在后台线程进行并投递到队列，主线程带超时取用——既避免管道写满死锁，
    也避免 readline 在残缺行上无限阻塞。
    """

    def __init__(self, argv, env=None, stderr_path=None, cwd=None):
        self.argv = list(argv)
        self.env = env
        self.cwd = str(cwd) if cwd else str(ROOT)
        self.stderr_path = Path(stderr_path) if stderr_path else None
        self.proc = None
        self._stderr_f = None
        self._q = queue.Queue()
        self._reader = None
        self._eof = False
        self._next_id = 1

    # ---- 生命周期 ----
    def start(self):
        stderr_target = subprocess.PIPE
        if self.stderr_path is not None:
            self._stderr_f = open(self.stderr_path, "wb")
            stderr_target = self._stderr_f
        self.proc = subprocess.Popen(
            self.argv,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=stderr_target,
            cwd=self.cwd,
            env=self.env,
        )
        self._reader = threading.Thread(target=self._pump, name="rpc-reader", daemon=True)
        self._reader.start()
        return self

    def _pump(self):
        try:
            for raw in iter(self.proc.stdout.readline, b""):
                self._q.put(raw)
        except Exception:  # noqa: BLE001 - 读取线程：失败即视为 EOF
            pass
        finally:
            self._eof = True
            self._q.put(None)  # 哨兵

    def close(self):
        try:
            if self.proc and self.proc.poll() is None:
                self.proc.terminate()
                try:
                    self.proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
                    self.proc.wait(timeout=3)
        finally:
            for stream in (getattr(self.proc, "stdin", None), getattr(self.proc, "stdout", None)):
                try:
                    if stream:
                        stream.close()
                except Exception:  # noqa: BLE001
                    pass
            if self._stderr_f:
                try:
                    self._stderr_f.close()
                except Exception:  # noqa: BLE001
                    pass

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.close()

    # ---- 进程状态 ----
    def alive(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def returncode(self):
        return self.proc.poll() if self.proc else None

    def wait_exit(self, timeout=5.0):
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None

    def drain_stderr(self) -> bytes:
        if not self.stderr_path or not self.stderr_path.exists():
            return b""
        return self.stderr_path.read_bytes()

    # ---- 传输 ----
    def send_raw(self, data: bytes):
        if isinstance(data, str):
            data = data.encode("utf-8")
        self.proc.stdin.write(data)
        self.proc.stdin.flush()

    def send_line(self, text: str):
        self.send_raw(text.encode("utf-8") + b"\n")

    def read_raw(self, timeout=RESP_TIMEOUT) -> bytes:
        try:
            item = self._q.get(timeout=timeout)
        except queue.Empty:
            raise RpcError(f"等待响应超时（{timeout}s）") from None
        if item is None:
            rc = self.returncode()
            raise RpcError(f"服务端 stdout 提前关闭（退出码 {rc}）")
        return item

    def read_json(self, timeout=RESP_TIMEOUT):
        raw = self.read_raw(timeout)
        return json.loads(raw.decode("utf-8"))

    def no_output_for(self, seconds: float):
        """在给定时窗内若无任何 stdout 输出则返回 None；否则返回原始字节。"""
        try:
            item = self._q.get(timeout=seconds)
        except queue.Empty:
            return None
        if item is None:
            raise RpcError("服务端在首个请求前就退出了（stdout EOF）")
        return item

    # ---- 请求 ----
    def request(self, obj, timeout=RESP_TIMEOUT):
        self.send_line(json.dumps(obj, ensure_ascii=False))
        return self.read_json(timeout)

    def call(self, method, params=None, id=None, timeout=RESP_TIMEOUT):
        if id is None:
            id = self._next_id
            self._next_id += 1
        obj = {"jsonrpc": "2.0", "id": id, "method": method}
        if params is not None:
            obj["params"] = params
        resp = self.request(obj, timeout=timeout)
        return resp


def _rpc_argv():
    override = os.environ.get("STARCOLONY_RPC_BIN")
    if override:
        p = Path(override)
        if not p.is_absolute():
            p = ROOT / p
        argv = [sys.executable, str(p)] if p.suffix == ".py" else [str(p)]
    else:
        argv = [str(DEFAULT_RPC)]
    extra = os.environ.get("STARCOLONY_RPC_ARGS", "").strip()
    if extra:
        argv += shlex.split(extra)
    return argv


def _server_available() -> bool:
    override = os.environ.get("STARCOLONY_RPC_BIN")
    if override:
        p = Path(override)
        if not p.is_absolute():
            p = ROOT / p
        return p.exists()
    return DEFAULT_RPC.exists()


requires_server = pytest.mark.skipif(
    not _server_available(),
    reason="被测服务端不存在（build/starcolony-rpc 未编译，或 STARCOLONY_RPC_BIN 指向的文件缺失）",
)

pytestmark = requires_server


# ---------------------------------------------------------------------------
#  fixtures
# ---------------------------------------------------------------------------
@pytest.fixture
def spawn():
    """子进程工厂：每个测试自建若干服务端，测试结束统一回收。"""
    clients: list[RpcClient] = []

    def _spawn(extra_env=None, cwd=None):
        env = os.environ.copy()
        env.setdefault("TMPDIR", str(BUILD / "tmp"))
        if extra_env:
            env.update(extra_env)
        stderr_path = STDERR_DIR / f"stderr-{uuid.uuid4().hex}.log"
        c = RpcClient(_rpc_argv(), env=env, stderr_path=stderr_path, cwd=cwd or ROOT)
        c.start()
        clients.append(c)
        return c

    yield _spawn
    for c in clients:
        c.close()


@pytest.fixture
def server(spawn):
    return spawn()


# ---------------------------------------------------------------------------
#  小工具
# ---------------------------------------------------------------------------
def R(resp):
    assert isinstance(resp, dict), resp
    assert "error" not in resp, f"预期正常响应，却收到协议错误：{resp}"
    assert "result" in resp, f"响应缺少 result：{resp}"
    return resp["result"]


def SNAP(resp):
    return R(resp)["snapshot"]


def CODE(resp):
    """取游戏内失败码（result.result.code）。"""
    r = R(resp)
    assert isinstance(r.get("result"), dict), f"缺少 result.result：{r}"
    return r["result"].get("code")


def canon(obj) -> str:
    """规范序列化——用于区分 true 与 1、保持字段次序无关。"""
    return json.dumps(obj, sort_keys=True, ensure_ascii=False)


def new_game(client, seed=None, name=None):
    params = {}
    if seed is not None:
        params["seed"] = seed
    if name is not None:
        params["name"] = name
    return client.call("new_game", params if params else None)


def command(client, action, **params):
    params = dict(params)
    params["action"] = action
    return client.call("command", params)


def free_plain(snapshot):
    """在 snapshot 里找一块可建普通建筑的空地（平原/冰层，且无建筑）。"""
    map_w, map_h, _ = parse_map_consts()
    for i, t in enumerate(snapshot["tiles"]):
        if t["building"] == -1 and t["terrain"] in (ord("."), ord("|")):
            return i % map_w, i // map_w
    return None, None


# ---------------------------------------------------------------------------
#  preview_build（§4.6.1）与新快照字段（§4.4）
# ---------------------------------------------------------------------------
def preview_build(client, key, x, y):
    return client.call("preview_build", {"key": key, "x": x, "y": y})


def bdef_map():
    """BDEF 的 key -> 行（含造价与 workers），用于「返回值必须与源码一致」类断言。"""
    return {b["key"]: b for b in parse_bdef()}


def cost_of(key):
    b = bdef_map()[key]
    return {"metal": b["costMetal"], "energy": b["costEnergy"], "science": b["costScience"]}


# 无人机网络（Tech::DroneNet）的 key：所有 workers>1 的建筑需求 -1
DRONENET_KEY = "drone"


def expected_worker_need(type_key, techs):
    """按源码规则算出该建筑**当前**的 workerNeed：def().workers，无人机网络 -1。"""
    n = bdef_map()[type_key]["workers"]
    if DRONENET_KEY in techs and n > 1:
        n -= 1
    return n


def drain_until_unaffordable(client, key="lab", max_builds=40):
    """反复建造把资源耗到不足以支付 key 的造价为止（**不推进周期**，纯靠建造扣费）。

    用于制造「地形可建但资源不足」这一前提——这正是 §4.6.1 要求
    buildable 与 affordable 相互独立的那个场景。
    """
    cost = cost_of(key)
    for _ in range(max_builds):
        snap = SNAP(client.call("snapshot"))
        if snap["metal"] < cost["metal"]:
            return snap
        x, y = free_plain(snap)
        if x is None:
            break
        res = R(command(client, "build", key=key, x=x, y=y))
        if not res["ok"]:
            break
    return SNAP(client.call("snapshot"))


def drive(clients, turns, before_turn=None):
    """把若干对局**同步**推进 turns 个周期；遇待决事件一律 answer(1)（两端同步）。

    before_turn 是可选钩子，在每个周期开始前对 clients[0] 施加额外动作
    （例如狂调 preview_build），用来检验该动作是否消耗随机数或改变状态。
    """
    for _ in range(turns):
        for c in clients:
            while True:
                if SNAP(c.call("snapshot"))["pending"] is None:
                    break
                R(command(c, "answer", option=1))
        if before_turn is not None:
            before_turn(clients[0])
        for c in clients:
            R(command(c, "advance"))


# ---- pending（§4.4.1）----
EVENT_KINDS = {
    "EventRefugees", "EventMarket", "EventSignal", "EventLifeSupport",
    "EventMeteorHit", "EventProspectFound", "EventVentFound", "EventFestival",
    "EventCaravan",
}


def advance_until_pending(client, max_turns=95):
    """推进直到出现待决事件；返回该 snapshot，超限/终局返回 None。"""
    for _ in range(max_turns):
        snap = R(command(client, "advance"))["snapshot"]
        if snap.get("pending") is not None:
            return snap
        if snap.get("over"):
            return None
    return None


def reach_pending(client, seeds=(1, 2, 3, 7, 11, 42, 99, 2024, 12345)):
    """扫若干固定 seed 造出一个待决事件（30%/周期，通常几回合即命中）。"""
    for s in seeds:
        new_game(client, seed=s)
        snap = advance_until_pending(client)
        if snap is not None:
            return snap
    return None


# ---------------------------------------------------------------------------
#  源码解析（用于 D 组：与真实源码核对，而非信任返回值）
# ---------------------------------------------------------------------------
def _read_src(name: str) -> str:
    return (SRC / name).read_text(encoding="utf-8")


def parse_map_consts():
    text = _read_src("types.hpp")

    def grab(name):
        m = re.search(r"inline constexpr int\s+%s\s*=\s*(-?\d+)" % name, text)
        assert m, f"types.hpp 未找到 {name}"
        return int(m.group(1))

    return grab("MAP_W"), grab("MAP_H"), grab("MAX_TURNS")


def parse_col_enum():
    text = _read_src("types.hpp")
    block = re.search(r"enum Col\s*:\s*int\s*\{(.*?)\}", text, re.S).group(1)
    vals, idx = {}, 0
    for m in re.finditer(r"(COL_\w+)\s*(?:=\s*(-?\d+))?", block):
        if m.group(2) is not None:
            idx = int(m.group(2))
        vals[m.group(1)] = idx
        idx += 1
    assert vals.get("COL_DEF") == 0 and vals.get("COL_BWHITE") == 9, vals
    return vals


_BDEF_RE = re.compile(
    r'\{\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*\'(.)\'\s*,\s*(COL_\w+)\s*,'
    r"\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,"
    r'\s*(true|false)\s*,\s*"([^"]*)"\s*\}'
)


def parse_bdef():
    text = _read_src("game.cpp")
    block = text.split("BDEF = {{", 1)[1].split("}};", 1)[0]
    rows = []
    for m in _BDEF_RE.finditer(block):
        k, name, glyph, col, M, E, Sc, bt, wk, up, rep, desc = m.groups()
        rows.append(
            dict(
                key=k, name=name, glyph=glyph, color=col,
                costMetal=int(M), costEnergy=int(E), costScience=int(Sc),
                buildTurns=int(bt), workers=int(wk), upkeep=int(up),
                repeatable=(rep == "true"), desc=desc,
            )
        )
    return rows


_TDEF_RE = re.compile(
    r'\{\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*(-?\d+)\s*,\s*([^,]+?)\s*,\s*"([^"]*)"\s*\}'
)


def parse_tech_enum():
    """按声明顺序返回 types.hpp 中 Tech 枚举的名字（不含 COUNT）。"""
    text = _read_src("types.hpp")
    block = re.search(r"enum class Tech\s*:\s*int\s*\{(.*?)\}", text, re.S).group(1)
    names = []
    for m in re.finditer(r"\b([A-Za-z_]\w*)\s*(?:=\s*\d+)?\s*,", block):
        if m.group(1) == "COUNT":
            break
        names.append(m.group(1))
    return names


def parse_tdef():
    text = _read_src("game.cpp")
    block = text.split("TDEF = {{", 1)[1].split("}};", 1)[0]
    raw = []
    for m in _TDEF_RE.finditer(block):
        k, name, cost, req, desc = m.groups()
        raw.append((k, name, int(cost), re.findall(r"Tech::(\w+)", req), desc))
    # 契约里的 req 是科技 key（小写，如 "fusion"），而源码写的是 C++ 枚举名（Tech::Fusion）。
    # TDEF 与 Tech 枚举同序，据此做 枚举名 -> key 的映射。
    enum_names = parse_tech_enum()
    name_to_key = {enum_names[i]: raw[i][0] for i in range(min(len(enum_names), len(raw)))}
    rows = []
    for k, name, cost, req_enums, desc in raw:
        rows.append(dict(key=k, name=name, cost=cost,
                         req=[name_to_key.get(e, e) for e in req_enums], desc=desc))
    return rows


_WDEF_RE = re.compile(
    r'\{\s*"([^"]*)"\s*,\s*(COL_\w+)\s*,\s*([\d.]+)\s*,\s*([\d.]+)\s*,'
    r'\s*([\d.]+)\s*,\s*([\d.]+)\s*,\s*"([^"]*)"\s*\}'
)


def parse_wdef():
    text = _read_src("game.cpp")
    block = text.split("WDEF = {{", 1)[1].split("}};", 1)[0]
    rows = []
    for m in _WDEF_RE.finditer(block):
        name, col, metal, energy, food, science, desc = m.groups()
        rows.append(dict(name=name, color=col, metal=float(metal), energy=float(energy),
                         food=float(food), science=float(science), desc=desc))
    return rows


# ===========================================================================
#  A. 启动与握手
# ===========================================================================
class TestA_Startup:
    def test_ping_returns_pong(self, server):
        resp = server.call("ping")
        assert resp["jsonrpc"] == "2.0"
        assert resp["id"] == 1
        assert R(resp)["ok"] is True
        assert R(resp)["pong"] is True

    def test_server_silent_before_first_request(self, server):
        # 协议 §1：服务端绝不主动推送任何内容到 stdout
        unexpected = server.no_output_for(0.6)
        assert unexpected is None, f"首个请求前 stdout 竟有输出：{unexpected!r}"
        # 空闲一段时间后仍无输出
        stale = server.no_output_for(0.4)
        assert stale is None, f"空闲期 stdout 有输出：{stale!r}"
        # 首次请求仍应正常
        assert R(server.call("ping"))["pong"] is True

    def test_no_unsolicited_output_between_requests(self, server):
        server.call("ping")
        server.call("new_game", {"seed": 1})
        for _ in range(3):
            server.call("snapshot")
        assert server.no_output_for(0.25) is None, "请求间出现了非响应的主动输出"


# ===========================================================================
#  B. new_game
# ===========================================================================
class TestB_NewGame:
    def test_with_seed_ok_and_full_snapshot(self, spawn):
        c = spawn()
        resp = new_game(c, seed=12345, name="新曙光")
        res = R(resp)
        assert res["ok"] is True
        snap = res["snapshot"]
        assert snap["turn"] == 1
        assert snap["colonyName"] == "新曙光"
        map_w, map_h, _ = parse_map_consts()
        assert len(snap["tiles"]) == map_w * map_h, "tiles 长度必须 = mapW*mapH"
        assert len(snap["buildings"]) >= 1, "新局至少应有指挥中心"
        assert all(k in snap for k in ("turn", "metal", "energy", "food", "science", "pop",
                                       "housing", "morale", "weather", "weatherLeft",
                                       "waveIn", "waveStrengthEstimate", "defense",
                                       "over", "won", "endReason", "colonyName",
                                       "buildings", "tiles", "log", "techs", "assigned")), \
            f"snapshot 字段不全：{sorted(snap)}"

    def test_same_seed_reproducible(self, spawn):
        a, b = spawn(), spawn()
        sa = SNAP(new_game(a, seed=12345, name="REPRO"))
        sb = SNAP(new_game(b, seed=12345, name="REPRO"))
        assert canon(sa) == canon(sb), "同 seed 两次 new_game 的 snapshot 不一致"

    def test_without_seed_ok(self, spawn):
        c = spawn()
        res = R(c.call("new_game"))
        assert res["ok"] is True and "snapshot" in res


# ===========================================================================
#  C. snapshot 纯查询
# ===========================================================================
class TestC_SnapshotPurity:
    def test_two_calls_identical(self, server):
        new_game(server, seed=7)
        s1 = SNAP(server.call("snapshot"))
        s2 = SNAP(server.call("snapshot"))
        assert canon(s1) == canon(s2), "连续两次 snapshot 不一致（非纯查询）"

    def test_does_not_consume_rng(self, spawn):
        """关键用例：狂调 snapshot 的一局 vs 不调的一局，推进后必须完全一致。"""
        seed = 2024
        a, b = spawn(), spawn()
        new_game(a, seed=seed, name="SNAP")
        new_game(b, seed=seed, name="SNAP")
        for _ in range(85):
            for _ in range(50):
                a.call("snapshot")
            command(a, "advance")
            command(b, "advance")
        sa = SNAP(a.call("snapshot"))
        sb = SNAP(b.call("snapshot"))
        assert canon(sa) == canon(sb), "snapshot 消耗了随机数：调与不调的两局状态分叉"


# ===========================================================================
#  D. content_info 与源码一致性
# ===========================================================================
class TestD_ContentInfo:
    def test_dimensions_match_types_hpp(self, server):
        map_w, map_h, max_turns = parse_map_consts()
        r = R(server.call("content_info"))
        assert r["ok"] is True
        assert r["mapW"] == map_w, f"mapW {r['mapW']} != MAP_W {map_w}"
        assert r["mapH"] == map_h, f"mapH {r['mapH']} != MAP_H {map_h}"
        assert r["maxTurns"] == max_turns, f"maxTurns {r['maxTurns']} != MAX_TURNS {max_turns}"

    def test_buildings_match_bdef(self, server):
        src = parse_bdef()
        cols = parse_col_enum()
        got = {b["key"]: b for b in R(server.call("content_info"))["buildings"]}
        assert len(got) == len(src), f"buildings 条目数 {len(got)} != BDEF 条数 {len(src)}"
        required = {"key", "name", "glyph", "color", "costMetal", "costEnergy",
                    "costScience", "buildTurns", "workers", "upkeep", "repeatable", "desc"}
        for row in src:
            assert row["key"] in got, f"缺少建筑 {row['key']}"
            g = got[row["key"]]
            assert required <= set(g), f"{row['key']} 字段不全：{sorted(g)}"
            assert g["name"] == row["name"], f"{row['key']} name"
            assert str(g["glyph"]) == row["glyph"], f"{row['key']} glyph"
            assert g["color"] == cols[row["color"]], f"{row['key']} color"
            for f in ("costMetal", "costEnergy", "costScience", "buildTurns", "workers", "upkeep"):
                assert g[f] == row[f], f"{row['key']}.{f}: {g[f]} != 源码 {row[f]}"
            assert g["repeatable"] is row["repeatable"], f"{row['key']} repeatable"
            assert g["desc"] == row["desc"], f"{row['key']} desc 与源码不一致"

    def test_buildings_spot_check(self, server):
        got = {b["key"]: b for b in R(server.call("content_info"))["buildings"]}
        gate = got["gate"]
        assert gate["name"] == "星门"
        assert gate["costMetal"] == 420 and gate["costEnergy"] == 300
        assert gate["workers"] == 8 and gate["buildTurns"] == 12
        assert gate["repeatable"] is False
        mine = got["mine"]
        assert mine["name"] == "钻矿场"
        assert (mine["costMetal"], mine["costEnergy"], mine["buildTurns"],
                mine["workers"], mine["upkeep"]) == (60, 10, 3, 3, 3)
        farm = got["farm"]
        assert farm["name"] == "水培农场"
        assert (farm["costMetal"], farm["costEnergy"], farm["buildTurns"],
                farm["workers"], farm["upkeep"]) == (50, 15, 3, 2, 2)

    def test_techs_match_tdef(self, server):
        src = parse_tdef()
        got = {t["key"]: t for t in R(server.call("content_info"))["techs"]}
        assert len(got) == len(src), f"techs 条目数 {len(got)} != TDEF 条数 {len(src)}"
        for row in src:
            assert row["key"] in got, f"缺少科技 {row['key']}"
            g = got[row["key"]]
            assert g["name"] == row["name"], f"{row['key']} name"
            assert g["cost"] == row["cost"], f"{row['key']} cost"
            assert set(g["req"]) == set(row["req"]), f"{row['key']} req: {g['req']} != {row['req']}"
            assert g["desc"] == row["desc"], f"{row['key']} desc 与源码不一致"

    def test_tech_spot_check(self, server):
        got = {t["key"]: t for t in R(server.call("content_info"))["techs"]}
        assert got["hydro"]["name"] == "水培改良" and got["hydro"]["cost"] == 35
        gate = got["gate"]
        assert gate["name"] == "星门理论" and gate["cost"] == 170
        assert set(gate["req"]) == {"fusion", "atmo"}, f"星门理论前置应为 fusion+atmo：{gate['req']}"

    def test_weathers_match_wdef(self, server):
        src = parse_wdef()
        cols = parse_col_enum()
        got = {w["name"]: w for w in R(server.call("content_info"))["weathers"]}
        assert len(got) == len(src), f"weathers 条目数 {len(got)} != WDEF 条数 {len(src)}"
        for row in src:
            assert row["name"] in got, f"缺少天气 {row['name']}"
            g = got[row["name"]]
            assert g["color"] == cols[row["color"]], f"{row['name']} color"
            for f in ("metal", "energy", "food", "science"):
                assert abs(float(g[f]) - row[f]) < 1e-9, f"{row['name']}.{f}: {g[f]} != {row[f]}"
            assert g["desc"] == row["desc"], f"{row['name']} desc 与源码不一致"

    def test_weather_spot_check(self, server):
        got = {w["name"]: w for w in R(server.call("content_info"))["weathers"]}
        clear = got["晴朗"]
        assert all(clear[f] == 1.0 for f in ("metal", "energy", "food", "science"))
        sand = got["沙暴"]
        assert sand["energy"] == 0.5, f"沙暴能源倍率应为 0.5，实际 {sand['energy']}"


# ===========================================================================
#  E. command 全 action
# ===========================================================================
class TestE_Command:
    def test_build_valid(self, server):
        new_game(server, seed=99)
        x, y = free_plain(SNAP(server.call("snapshot")))
        assert x is not None, "找不到可建空地"
        res = R(command(server, "build", key="sol", x=x, y=y))
        assert res["ok"] is True, res
        assert res["result"]["code"] == "BuildStarted", res["result"]
        assert "log" in res and "snapshot" in res, "command 返回必须同时含 log 与 snapshot"

    def test_build_out_of_bounds_keeps_alive(self, server):
        new_game(server, seed=99)
        res = R(command(server, "build", key="sol", x=9999, y=9999))
        assert res["ok"] is False
        assert res["result"]["code"] == "BuildBlocked", res["result"]
        assert R(server.call("ping"))["pong"] is True, "越界请求后进程不应死亡"

    def test_build_unknown_key_keeps_alive(self, server):
        new_game(server, seed=99)
        res = R(command(server, "build", key="zzz_nope", x=0, y=0))
        assert res["ok"] is False
        assert res["result"]["code"] == "BuildUnknownType", res["result"]
        assert R(server.call("ping"))["pong"] is True

    def test_demolish_invalid_ids(self, server):
        new_game(server, seed=99)
        for bad in (-1, 9999):
            res = R(command(server, "demolish", id=bad))
            assert res["ok"] is False, (bad, res)
            assert res["result"]["code"] == "DemolishInvalid", (bad, res["result"])
        # 建一个再拆，重复拆同一 id 必须失败
        x, y = free_plain(SNAP(server.call("snapshot")))
        command(server, "build", key="sol", x=x, y=y)
        snap = SNAP(server.call("snapshot"))
        bid = next(b["id"] for b in snap["buildings"] if b["x"] == x and b["y"] == y)
        first = R(command(server, "demolish", id=bid))
        assert first["ok"] is True and first["result"]["code"] == "DemolishDone", first
        again = R(command(server, "demolish", id=bid))
        assert again["ok"] is False and again["result"]["code"] == "DemolishInvalid", again

    def test_research_failure_codes(self, server):
        new_game(server, seed=5)
        assert CODE(command(server, "research", key="zzz_nope")) == "ResearchUnknown"
        assert CODE(command(server, "research", key="atmo")) == "ResearchPrereq"   # 缺前置 hydro
        assert CODE(command(server, "research", key="hydro")) == "ResearchNoScience"  # 科研 0

    def test_answer_no_pending(self, server):
        new_game(server, seed=5)
        res = R(command(server, "answer", option=1))
        assert res["ok"] is False
        assert res["result"]["code"] == "AnswerNone", res["result"]

    def test_advance_increments_turn(self, server):
        t0 = SNAP(new_game(server, seed=5))["turn"]
        res = R(command(server, "advance"))
        assert res["snapshot"]["turn"] == t0 + 1, (t0, res["snapshot"]["turn"])
        assert "log" in res and "snapshot" in res

    def test_toggle_success_and_invalid(self, server):
        new_game(server, seed=5)
        assert CODE(command(server, "toggle", id=9999)) == "ToggleInvalid"
        on = R(command(server, "toggle", id=0))
        assert on["ok"] is True and on["result"]["code"] == "ToggleDone", on["result"]
        assert on["snapshot"]["buildings"][0]["enabled"] is False, "toggle 应关闭建筑"
        off = R(command(server, "toggle", id=0))
        assert off["ok"] is True and off["result"]["code"] == "ToggleDone"
        assert off["snapshot"]["buildings"][0]["enabled"] is True, "再次 toggle 应恢复"

    def test_focus_success_and_invalid(self, server):
        new_game(server, seed=5)
        assert CODE(command(server, "focus", id=9999)) == "FocusInvalid"
        r = R(command(server, "focus", id=0))
        assert r["ok"] is True and r["result"]["code"] == "FocusDone", r["result"]
        assert "log" in r and "snapshot" in r

    def test_demolish_hq_rejected(self, server):
        new_game(server, seed=5)
        res = R(command(server, "demolish", id=0))
        assert res["ok"] is False and res["result"]["code"] == "DemolishHQ", res["result"]

    def test_unknown_action_not_silently_ok(self, server):
        new_game(server, seed=5)
        resp = command(server, "frobnicate")
        if "error" in resp:
            assert isinstance(resp["error"].get("code"), int) and resp["error"]["code"] < 0, resp
        else:
            assert R(resp)["ok"] is False, "未知 action 不得静默成功"
        assert R(server.call("ping"))["pong"] is True


# ===========================================================================
#  F. 协议健壮性（对抗性）
# ===========================================================================
# 注意：空行从严格清单中剔除——契约 §1/§2 并未规定空行策略（见 test_blank_line_handling）。
_BAD_JSON = [b"{", b"not json", b"[1,2,3", b'{"jsonrpc":"2.0",',
             b"undefined", b"NaN", b"Infinity", b"\x00\x01\x02"]


class TestF_Robustness:
    @pytest.mark.parametrize("bad", _BAD_JSON, ids=[repr(b) for b in _BAD_JSON])
    def test_invalid_json_returns_parse_error(self, server, bad):
        # 每条畸形输入都必须返回 -32700 解析错误（协议 §2），且进程存活。
        server.call("ping")  # 建立状态
        server.send_raw(bad + b"\n")
        resp = server.read_json()
        assert "error" in resp, f"{bad!r} 未返回错误：{resp}"
        assert resp["error"]["code"] == -32700, f"{bad!r} 应为 -32700：{resp}"
        assert server.alive(), f"{bad!r} 之后进程死亡"
        assert R(server.call("ping"))["pong"] is True, f"{bad!r} 之后无法继续服务"

    def test_blank_line_is_silently_ignored(self, server):
        # §1.1（已定死）：空行不是请求，服务端静默忽略、不产生任何响应
        server.call("ping")
        server.send_raw(b"\n")
        assert server.no_output_for(0.4) is None, "空行必须无响应（§1.1）"
        assert server.alive()

    def test_blank_line_does_not_break_pairing(self, server):
        # 构造 ping → 空行 → ping → 空行 → new_game，响应序列必须恰为这 3 个请求、顺序与 id 对得上
        ping1 = server.call("ping")
        assert ping1["id"] == 1 and R(ping1)["pong"] is True, ping1
        server.send_raw(b"\n")
        ping2 = server.call("ping")
        assert ping2.get("id") == 2, f"空行后响应错位：{ping2}"
        assert R(ping2)["pong"] is True, ping2
        server.send_raw(b"\n")
        ng = server.call("new_game", {"seed": 3})
        assert ng.get("id") == 3 and R(ng)["ok"] is True, f"空行后响应错位：{ng}"
        assert server.no_output_for(0.3) is None, "出现了多余的响应"

    def test_very_long_invalid_line(self, server):
        server.call("ping")
        server.send_raw(b"x" * 200_000 + b"\n")
        resp = server.read_json()
        assert "error" in resp, f"超长非法行未返回错误：{resp}"
        assert isinstance(resp["error"]["code"], int) and resp["error"]["code"] < 0
        assert server.alive(), "超长行之后进程死亡"
        assert R(server.call("ping"))["pong"] is True

    def test_missing_method(self, server):
        resp = server.request({"jsonrpc": "2.0", "id": 1})
        assert "error" in resp and resp["error"]["code"] == -32600, resp

    def test_method_not_string(self, server):
        resp = server.request({"jsonrpc": "2.0", "id": 2, "method": 123})
        assert "error" in resp, resp
        assert resp["error"]["code"] in (-32600, -32601), resp
        assert server.alive()

    def test_id_object_returns_invalid_request(self, server):
        # §1.1：id 为对象/数组 → -32600，且响应 id 置 null
        resp = server.request({"jsonrpc": "2.0", "id": {"nested": 1}, "method": "ping"})
        assert "error" in resp, resp
        assert resp["error"]["code"] == -32600, resp
        assert resp["id"] is None, f"非法 id 必须回填 null：{resp}"
        assert server.alive()
        assert R(server.call("ping"))["pong"] is True

    def test_id_array_returns_invalid_request(self, server):
        resp = server.request({"jsonrpc": "2.0", "id": [1, 2], "method": "ping"})
        assert "error" in resp and resp["error"]["code"] == -32600, resp
        assert resp["id"] is None, resp
        assert server.alive()

    @pytest.mark.parametrize("rid", ["str-id", 7, None], ids=["string", "number", "null"])
    def test_id_valid_types_echoed(self, server, rid):
        # §1.1：string / number / null 原样回填
        resp = server.request({"jsonrpc": "2.0", "id": rid, "method": "ping"})
        assert R(resp)["pong"] is True, resp
        assert resp["id"] == rid, f"合法 id 应原样回填：{resp}"

    def test_id_missing_treated_as_normal_request(self, server):
        # §1.1：id 缺失 → 按正常请求处理（不得报错）
        resp = server.request({"jsonrpc": "2.0", "method": "ping"})
        assert R(resp)["pong"] is True, resp
        assert server.alive()

    def test_unknown_method_32601(self, server):
        resp = server.call("no_such_method")
        assert "error" in resp and resp["error"]["code"] == -32601, resp

    def test_params_type_error(self, server):
        new_game(server, seed=5)
        resp = command(server, "build", key="sol", x="notanint", y=0)
        if "error" in resp:
            assert resp["error"]["code"] in (-32600, -32602, -32700), resp
        else:
            assert R(resp)["ok"] is False, "参数类型错误不得静默成功"
        assert server.alive(), "参数类型错误后进程死亡"
        assert R(server.call("ping"))["pong"] is True

    def test_utf8_roundtrip_byte_exact(self, spawn):
        c = spawn()
        names = [
            "铁砧",
            '引号"里面"',
            "反斜杠\\路径",
            "换行\n第二行",
            "制表\t符",
            "混合 中文 + emoji 🚀 + ünïcödé",
            "换行\r回车",
        ]
        for nm in names:
            res = R(new_game(c, seed=1, name=nm))
            assert res["ok"] is True
            got = res["snapshot"]["colonyName"]
            assert got == nm, f"UTF-8 往返不一致：期望 {nm!r}，得到 {got!r}"
        assert c.alive()

    def test_1000_requests_in_order(self, server):
        server.call("ping")
        n = ORDER_N
        for i in range(n):
            server.send_raw(json.dumps({"jsonrpc": "2.0", "id": i, "method": "ping"}).encode("utf-8") + b"\n")
        ids = []
        for _ in range(n):
            resp = server.read_json(timeout=RESP_TIMEOUT * 3)
            ids.append(resp.get("id"))
        assert ids == list(range(n)), "响应顺序与请求不一致（或出现多余/缺失响应）"


# ===========================================================================
#  G. save / load
# ===========================================================================
def _snapshot_without_log(snap, drop=()):
    """比较用：剔除非语义字段。

    默认剔 `log` —— 引擎读档后日志条目一律退化为 Text（game.cpp:1072），
    code 必然不同，那是已知且已记录的行为，不是回归。
    涉及 `load` 的比较额外剔 `seed`：§4.4 规定读档后 seed==0（存档不含种子），
    该行为由 TestN_SnapshotFields.test_seed_zero_after_load 单独钉死。
    """
    s = dict(snap)
    s.pop("log", None)
    for k in drop:
        s.pop(k, None)
    return s


def _log_texts(snap):
    return [e.get("text") for e in snap.get("log", [])]


class TestG_SaveLoad:
    def test_save_load_roundtrip(self, spawn):
        c = spawn()
        new_game(c, seed=42, name="SAVELOAD")
        for _ in range(5):
            command(c, "advance")
        before = SNAP(c.call("snapshot"))
        path = str(TMP / f"qa_roundtrip_{uuid.uuid4().hex}.sav")

        res = R(c.call("save", {"path": path}))
        assert res["ok"] is True, res
        # save 不应改变对局
        assert canon(SNAP(c.call("snapshot"))) == canon(before), "save 改变了当前对局状态"

        # 推进几步后再读档，必须回到存档时刻
        command(c, "advance")
        res = R(c.call("load", {"path": path}))
        assert res["ok"] is True, res
        restored = SNAP(c.call("snapshot"))
        # 注：引擎读档后日志条目一律退化为 Text（game.cpp:1072），故日志 code 必然不同；
        #     这里比较除 log 之外的全部字段 + 日志文本内容（seed 按 §4.4 读档后为 0，故剔除）。
        assert canon(_snapshot_without_log(restored, drop=("seed",))) == \
               canon(_snapshot_without_log(before, drop=("seed",))), \
            "save->load 后 snapshot 不一致"
        assert _log_texts(restored) == _log_texts(before), "读档后日志文本不一致"

    def test_load_missing_fails_and_preserves_state(self, server):
        new_game(server, seed=42, name="KEEP")
        command(server, "advance")
        before = SNAP(server.call("snapshot"))
        missing = str(TMP / f"qa_missing_{uuid.uuid4().hex}.sav")
        res = R(server.call("load", {"path": missing}))
        assert res["ok"] is False, res
        assert res["result"]["code"] == "LoadFailed", res["result"]
        after = SNAP(server.call("snapshot"))
        assert canon(after) == canon(before), "读档失败破坏了原对局状态"
        assert server.alive()

    def test_save_load_save_byte_identical(self, spawn):
        c = spawn()
        new_game(c, seed=77, name="RTOUND")
        for _ in range(10):
            command(c, "advance")
        a = str(TMP / f"qa_rt_a_{uuid.uuid4().hex}.sav")
        b = str(TMP / f"qa_rt_b_{uuid.uuid4().hex}.sav")
        assert R(c.call("save", {"path": a}))["ok"] is True
        assert R(c.call("load", {"path": a}))["ok"] is True
        assert R(c.call("save", {"path": b}))["ok"] is True
        assert Path(a).read_bytes() == Path(b).read_bytes(), "save->load->save 字节不一致"


    def test_save_load_default_path(self, spawn):
        # path 缺省时由服务端沿用引擎默认存档名；用 cwd=build/qa_tmp 避免污染仓库根
        c = spawn(cwd=TMP)
        new_game(c, seed=3, name="DEFPATH")
        before = SNAP(c.call("snapshot"))
        r = R(c.call("save"))
        assert r["ok"] is True, r
        command(c, "advance")
        r2 = R(c.call("load"))
        assert r2["ok"] is True, r2
        restored = SNAP(c.call("snapshot"))
        assert canon(_snapshot_without_log(restored, drop=("seed",))) == \
               canon(_snapshot_without_log(before, drop=("seed",))), \
            "缺省 path 的 save->load 后 snapshot 不一致"


# ===========================================================================
#  J. snapshot.pending —— 待决事件（§4.4.1）
# ===========================================================================
class TestJ_Pending:
    def test_pending_present_and_null_without_event(self, server):
        snap = SNAP(new_game(server, seed=12345))
        assert "pending" in snap, "snapshot 必须包含 pending 字段（§4.4.1）"
        assert snap["pending"] is None, "新局无待决事件时 pending 应为 null"

    def test_pending_shape_when_event(self, spawn):
        c = spawn()
        snap = reach_pending(c)
        assert snap is not None, "候选 seed 内未造出待决事件"
        p = snap["pending"]
        assert isinstance(p, dict), p
        assert p.get("kind") in EVENT_KINDS, f"kind 非法的待决事件：{p.get('kind')}"
        assert isinstance(p.get("title"), str) and p["title"], "title 应为非空字符串"
        assert isinstance(p.get("text"), str), "text 应为字符串"
        assert isinstance(p.get("options"), list) and len(p["options"]) >= 1, "options 应非空数组"
        assert all(isinstance(o, str) and o for o in p["options"]), "options 元素应为非空字符串"

    def test_answer_option1_succeeds(self, spawn):
        c = spawn()
        snap = reach_pending(c)
        assert snap is not None, "未造出待决事件"
        res = R(command(c, "answer", option=1))
        assert res["ok"] is True, res
        assert res["result"]["code"] == "AnswerChoice", res["result"]
        assert "log" in res and "snapshot" in res
        nxt = res["snapshot"]["pending"]
        assert nxt is None or isinstance(nxt, dict), "应答后 pending 应为 null 或下一个事件"

    def test_answer_out_of_range_fails(self, spawn):
        c = spawn()
        snap = reach_pending(c)
        assert snap is not None, "未造出待决事件"
        n = len(snap["pending"]["options"])
        for bad in (0, n + 1, -1):
            res = R(command(c, "answer", option=bad))
            assert res["ok"] is False, (bad, res)
            assert res["result"]["code"] == "AnswerInvalid", (bad, res["result"])
            assert res["snapshot"]["pending"] is not None, "失败应答不得消耗待决事件"

    def test_non_answer_action_rejected_during_pending(self, spawn):
        """§4.4.1 声称「引擎实际也会拒绝其它操作」。本用例把该主张编码为强断言：
        若待决期间 build / advance 被接受，本用例变红 = 契约与引擎不符，须上报（不替引擎圆场）。"""
        c = spawn()
        snap = reach_pending(c)
        assert snap is not None, "未造出待决事件"
        x, y = free_plain(snap)
        res = R(command(c, "build", key="sol", x=x, y=y))
        assert res["ok"] is False, f"待决期间 build 不应成功（§4.4.1）：{res}"
        adv = R(command(c, "advance"))
        assert adv["ok"] is False, f"待决期间 advance 不应成功（§4.4.1）：{adv}"
        assert c.alive()

    def test_all_non_answer_actions_blocked_by_pending(self, spawn):
        """§4.4.1（core 强制）：待决期间 build/demolish/toggle/focus/research/advance
        六个 action 必须**全部** ok=false + code=="BlockedByPending"（不只 build/advance 两个）。"""
        c = spawn()
        snap = reach_pending(c)
        assert snap is not None, "未造出待决事件"
        x, y = free_plain(snap)
        cases = {
            "build":    dict(key="sol", x=x, y=y),
            "demolish": dict(id=0),
            "toggle":   dict(id=0),
            "focus":    dict(id=0),
            "research": dict(key="hydro"),
            "advance":  {},
        }
        for action, params in cases.items():
            res = R(command(c, action, **params))
            assert res["ok"] is False, f"待决期间 {action} 不应成功：{res}"
            assert res["result"]["code"] == "BlockedByPending", \
                f"待决期间 {action} 的 code 应为 BlockedByPending，实际 {res['result'].get('code')!r}"
            assert res["snapshot"]["pending"] is not None, f"被拒的 {action} 不应消耗待决事件"
        assert c.alive()

    def test_blocked_advance_leaves_state_unchanged(self, spawn):
        """§4.4.1：advance 被拒后状态必须**完全不变**（防「先推进再回滚」歪招）。"""
        c = spawn()
        snap = reach_pending(c)
        assert snap is not None, "未造出待决事件"
        before = SNAP(c.call("snapshot"))
        res = R(command(c, "advance"))
        assert res["ok"] is False and res["result"]["code"] == "BlockedByPending", res
        after = SNAP(c.call("snapshot"))
        for f in ("turn", "metal", "energy", "food", "science", "pop", "housing",
                  "morale", "weather", "weatherLeft", "waveIn", "defense",
                  "over", "won", "endReason", "pending"):
            assert after[f] == before[f], f"advance 被拒却改变了 {f}：{before[f]!r} -> {after[f]!r}"
        assert canon(after) == canon(before), "advance 被拒后 snapshot 不应有任何变化"


# ===========================================================================
#  K. 长会话 / 终局 / 大快照
# ===========================================================================
def _advance_to_end(c, max_steps=240):
    snap = None
    for _ in range(max_steps):
        snap = R(command(c, "advance"))["snapshot"]
        if snap["pending"] is not None:          # 遇事件先应答，否则某些实现会卡住推进
            R(command(c, "answer", option=1))
            snap = SNAP(c.call("snapshot"))
        if snap["over"]:
            return snap
    return snap


class TestK_LongSession:
    def test_advance_to_end_then_freeze(self, spawn):
        c = spawn()
        new_game(c, seed=2024, name="LONG")
        snap = _advance_to_end(c)
        assert snap is not None and snap["over"] is True, "90 周期内未终局"
        assert isinstance(snap["endReason"], str) and snap["endReason"], "终局应给出非空 endReason"
        assert isinstance(snap["won"], bool)
        assert len(snap["log"]) <= 400, f"log 超过引擎上限 400：{len(snap['log'])}"
        # 终局后 advance 不再改变状态
        frozen = canon(SNAP(c.call("snapshot")))
        command(c, "advance")
        assert canon(SNAP(c.call("snapshot"))) == frozen, "终局后 advance 不应改变任何状态"

    def test_large_snapshot_late_game(self, spawn):
        c = spawn()
        new_game(c, seed=777, name="BIG")
        _advance_to_end(c)
        snap = SNAP(c.call("snapshot"))
        map_w, map_h, _ = parse_map_consts()
        assert len(snap["tiles"]) == map_w * map_h, "后期 tiles 长度应仍为 mapW*mapH"
        assert len(snap["buildings"]) >= 1
        assert len(snap["log"]) <= 400
        assert isinstance(snap["assigned"], list)
        # 大对象往返编解码无截断（read_json 已解析，这里再自校验一次）
        assert json.loads(json.dumps(snap, ensure_ascii=False)) == snap


# ===========================================================================
#  L. validate（§4.8）
# ===========================================================================
class TestL_Validate:
    def test_validate_ok_empty_errors(self, server):
        r = R(server.call("validate", {"path": "no_such.sav"}))
        assert r.get("ok") is True, r
        assert r.get("errors") == [], r


# ===========================================================================
#  M. preview_build —— 建造可行性只读查询（§4.6.1）
# ===========================================================================
class TestM_PreviewBuild:
    def test_buildable_and_affordable(self, server):
        """① 可建 + 可付：cost 三项须与 content_info 一致，affordable 全 true。"""
        new_game(server, seed=99, name="PREVIEW")
        snap = SNAP(server.call("snapshot"))
        x, y = free_plain(snap)
        assert x is not None, "找不到可建空地"
        r = R(preview_build(server, "sol", x, y))
        assert r["ok"] is True, r
        assert r["buildable"] is True, f"空地上的太阳能板应可建：{r}"
        assert r["reason"] == "", f"可建时 reason 必须为空串：{r['reason']!r}"
        ci = {b["key"]: b for b in R(server.call("content_info"))["buildings"]}["sol"]
        assert r["cost"] == {"metal": ci["costMetal"], "energy": ci["costEnergy"],
                              "science": ci["costScience"]}, \
            f"cost 必须来自 BDEF：{r['cost']} != {ci}"
        assert r["cost"] == cost_of("sol")
        assert r["affordable"] == {"metal": True, "energy": True, "science": True}, r["affordable"]
        # affordable 必须由服务端按当前资源算出
        assert r["affordable"]["metal"] == (snap["metal"] >= r["cost"]["metal"])
        assert r["affordable"]["energy"] == (snap["energy"] >= r["cost"]["energy"])
        assert r["affordable"]["science"] == (snap["science"] >= r["cost"]["science"])

    def test_terrain_mismatch_not_buildable(self, server):
        """② 地形不符：buildable=false 且 reason 非空，且与 command{build} 失败原因同源。"""
        new_game(server, seed=99, name="TERRAIN")
        snap = SNAP(server.call("snapshot"))
        x, y = free_plain(snap)          # 平原/冰层：钻矿场必须建在矿脉上
        assert x is not None
        r = R(preview_build(server, "mine", x, y))
        assert r["ok"] is True, r
        assert r["buildable"] is False, f"平地上不能建钻矿场，buildable 应为 false：{r}"
        assert isinstance(r["reason"], str) and r["reason"], \
            f"不可建时必须给出非空 reason（供前端直接灰显）：{r}"
        bad = R(command(server, "build", key="mine", x=x, y=y))
        assert bad["ok"] is False and bad["result"]["code"] == "BuildBlocked", bad
        assert r["reason"] in bad["result"]["text"], \
            f"reason 与 command{{build}} 的失败原因应同源：{r['reason']!r} vs {bad['result']['text']!r}"

    def test_buildable_true_but_unaffordable(self, spawn):
        """③ **两者相互独立**（最容易实现错的一条）：资源不足时 buildable 仍为 true、
        reason 仍为空，而 affordable 的对应项为 false。

        注意引擎的 Game::buildable() 把资源检查也算进去（会返回「金属不足」），
        所以这里专门钉的是 §4.6.1：地形/占用/前置层面的可建性不得受资源影响。
        """
        c = spawn()
        new_game(c, seed=99, name="POOR")
        snap = drain_until_unaffordable(c, "lab")
        cost = cost_of("lab")
        assert snap["metal"] < cost["metal"], \
            f"前提不成立：金属 {snap['metal']} 仍够付 {cost['metal']}，造不出「资源不足」场景"
        x, y = free_plain(snap)
        assert x is not None
        r = R(preview_build(c, "lab", x, y))
        assert r["ok"] is True, r
        assert r["buildable"] is True, \
            f"资源不足不得影响 buildable（§4.6.1 两者独立），实际 buildable={r['buildable']!r} reason={r.get('reason')!r}"
        assert r["reason"] == "", \
            f"资源不足不应写进 reason（那是 affordable 的职责），实际 {r['reason']!r}"
        assert r["cost"] == cost
        assert r["affordable"]["metal"] is False, \
            f"金属 {snap['metal']} < {cost['metal']}，affordable.metal 应为 false：{r['affordable']}"
        # 其余两项仍须按真实资源如实计算
        assert r["affordable"]["energy"] == (snap["energy"] >= cost["energy"]), r["affordable"]
        assert r["affordable"]["science"] == (snap["science"] >= cost["science"]), r["affordable"]

    def test_unknown_key(self, server):
        """④ 未知 key → BuildUnknownType（与 command{build} 一致，便于前端统一处理）。"""
        new_game(server, seed=99, name="UNKNOWN")
        r = R(preview_build(server, "zzz_nope", 0, 0))
        assert r["ok"] is False, r
        assert r["result"]["code"] == "BuildUnknownType", r["result"]
        assert server.alive()
        assert R(server.call("ping"))["pong"] is True

    @pytest.mark.parametrize("bad_xy", [(-1, 0), (0, -1), (9999, 9999)],
                             ids=["x=-1", "y=-1", "far"])
    def test_out_of_bounds(self, server, bad_xy):
        """⑤ 坐标越界 → BuildBlocked，且原因可被前端直接显示。"""
        new_game(server, seed=99, name="OOB")
        r = R(preview_build(server, "sol", *bad_xy))
        assert r["ok"] is False, r
        assert r["result"]["code"] == "BuildBlocked", r["result"]
        blob = canon(r)
        assert ("越界" in blob or "范围" in blob), f"越界原因应可被前端直接显示：{r}"
        assert server.alive()

    def test_blocked_during_pending(self, spawn):
        """⑥ 待决事件期间 → BlockedByPending（§4.4.1 同样约束 preview_build）。"""
        c = spawn()
        snap = reach_pending(c)
        assert snap is not None, "未造出待决事件"
        x, y = free_plain(snap)
        r = R(preview_build(c, "sol", x, y))
        assert r["ok"] is False, f"待决期间 preview_build 不应成功（§4.4.1/§4.6.1）：{r}"
        assert r["result"]["code"] == "BlockedByPending", r["result"]
        assert c.alive()

    def test_fifty_calls_leave_snapshot_identical(self, server):
        """⑦ 纯查询性：同坐标连调 50 次，快照必须逐字相同（含 turn / 资源 / pending）。"""
        new_game(server, seed=31, name="PURE")
        x, y = free_plain(SNAP(server.call("snapshot")))
        before = SNAP(server.call("snapshot"))
        for _ in range(50):
            R(preview_build(server, "sol", x, y))
        after = SNAP(server.call("snapshot"))
        assert canon(after) == canon(before), "preview_build 改变了 snapshot（应为零副作用）"
        assert R(server.call("ping"))["pong"] is True

    def test_does_not_consume_rng(self, spawn):
        """⑦ 续：狂调 preview_build 的对局 vs 不调的对局，同 seed 推进 85 回合后状态一致。"""
        seed = 2024
        a, b = spawn(), spawn()
        new_game(a, seed=seed, name="PREVPURE")
        new_game(b, seed=seed, name="PREVPURE")

        def spam(c):
            x, y = free_plain(SNAP(c.call("snapshot")))
            if x is None:
                return
            for _ in range(50):
                c.call("preview_build", {"key": "sol", "x": x, "y": y})

        drive([a, b], 85, before_turn=spam)
        sa = SNAP(a.call("snapshot"))
        sb = SNAP(b.call("snapshot"))
        assert sa["turn"] > 1, "推进空转（turn 未前进），本用例的对比不成立"
        assert canon(sa) == canon(sb), \
            "preview_build 消耗了随机数或改动了状态：狂调与不调的两局状态分叉"


# ===========================================================================
#  N. snapshot 新字段：idleWorkers / seed / buildings[].workerNeed（§4.4）
# ===========================================================================
class TestN_SnapshotFields:
    def test_idle_workers_int_and_consistent(self, server):
        """idleWorkers：整数，且 = max(0, pop - Σassigned)；初始局面 6 人口 / HQ 需 2 → 4。"""
        snap = SNAP(new_game(server, seed=42, name="FIELDS"))
        assert "idleWorkers" in snap, "§4.4：snapshot 必须包含 idleWorkers"
        iw = snap["idleWorkers"]
        assert isinstance(iw, int) and not isinstance(iw, bool), \
            f"idleWorkers 必须是整数（不是 bool/null），实际 {iw!r}"
        assert iw >= 0, f"idleWorkers 不得为负：{iw}"
        assert iw == max(0, snap["pop"] - sum(snap["assigned"])), \
            f"idleWorkers {iw} != pop{ snap['pop'] } - Σassigned{sum(snap['assigned'])}"
        assert snap["pop"] == 6, f"初始人口应为 6，实际 {snap['pop']}"
        assert iw == 4, f"初始局面（6 人口、HQ 需 2 人）闲置应为 4，实际 {iw}"

    def test_seed_echoed_when_given(self, server):
        snap = SNAP(new_game(server, seed=42, name="SEED"))
        assert "seed" in snap, "§4.4：snapshot 必须包含 seed"
        assert isinstance(snap["seed"], int) and not isinstance(snap["seed"], bool), snap["seed"]
        assert snap["seed"] == 42, f"new_game{{seed:42}} 后 seed 应回显 42，实际 {snap['seed']}"

    def test_seed_positive_when_omitted(self, spawn):
        """省略 seed 时服务端自取时间种子：必须 >0（前端仅在 seed>0 时显示）。"""
        for c in (spawn(), spawn()):
            snap = SNAP(new_game(c))
            assert isinstance(snap["seed"], int) and not isinstance(snap["seed"], bool), snap["seed"]
            assert snap["seed"] > 0, f"省略 seed 时 seed 应 >0（0 保留给「未知」），实际 {snap['seed']}"

    def test_seed_zero_after_load(self, spawn):
        """§4.4：P2 不动存档格式，故读档后 seed 必须为 0（未知）。

        这条同时防一个陷阱：进程内先 new_game{seed:42} 再 load，若服务端只是
        「不更新 seed」而不显式清零，snapshot 会继续报 42 —— 那是错的。
        """
        c = spawn()
        new_game(c, seed=42, name="SEEDLOAD")
        command(c, "advance")
        path = str(TMP / f"qa_seed_{uuid.uuid4().hex}.sav")
        assert R(c.call("save", {"path": path}))["ok"] is True
        assert SNAP(c.call("snapshot"))["seed"] == 42, "save 不得改变 seed"
        command(c, "advance")
        assert R(c.call("load", {"path": path}))["ok"] is True
        after = SNAP(c.call("snapshot"))
        assert after["seed"] == 0, \
            f"§4.4：读档后 seed 必须为 0（存档不含种子），实际 {after['seed']}"

    def test_workerneed_present_and_matches_source(self, spawn):
        """buildings[].workerNeed：存在、非负、且等于源码算出的当前需求（含无人机网络修正）。"""
        c = spawn()
        new_game(c, seed=42, name="WN")
        for key in ("sol", "farm", "lab", "turret"):
            x, y = free_plain(SNAP(c.call("snapshot")))
            if x is None:
                break
            R(command(c, "build", key=key, x=x, y=y))
        snap = SNAP(c.call("snapshot"))
        known = bdef_map()
        assert snap["buildings"], "至少应有指挥中心"
        for b in snap["buildings"]:
            assert "workerNeed" in b, f"buildings[] 必须含 workerNeed：{b}"
            wn = b["workerNeed"]
            assert isinstance(wn, int) and not isinstance(wn, bool), \
                f"workerNeed 必须是非负整数（0 是合法值）：{b}"
            assert wn >= 0, f"workerNeed 不得为负：{b}"
            if b["typeKey"] in known:
                assert wn == expected_worker_need(b["typeKey"], snap["techs"]), \
                    f"{b['typeKey']} 的 workerNeed 应为 " \
                    f"{expected_worker_need(b['typeKey'], snap['techs'])}，实际 {wn}"

    def test_workerneed_zero_not_treated_as_missing(self, spawn):
        """workerNeed==0 必须如实上报 0，不能被当成「缺失」而省略或置 null。"""
        c = spawn()
        new_game(c, seed=42, name="WN0")
        for key in ("sol", "farm", "hab", "lab", "clinic", "turret"):
            x, y = free_plain(SNAP(c.call("snapshot")))
            if x is None:
                break
            R(command(c, "build", key=key, x=x, y=y))
        snap = SNAP(c.call("snapshot"))
        for b in snap["buildings"]:
            assert "workerNeed" in b, f"buildings[] 必须含 workerNeed 字段：{b}"
            wn = b["workerNeed"]
            assert isinstance(wn, int) and not isinstance(wn, bool), \
                f"workerNeed 必须是非负整数（0 合法，不得当缺失）：{b}"
            assert wn >= 0, f"workerNeed 不得为负：{b}"
        zeros = [b for b in snap["buildings"] if b["workerNeed"] == 0]
        if not zeros:
            pytest.skip(
                "当前内容不存在 workerNeed==0 的建筑：BDEF 最小 workers=1，"
                "无人机网络只把 >1 减 1（下限仍是 1），故真服务端无法触发该分支；"
                "该性质由 mock 的一座 0 工人建筑（beacon）验证。"
            )
        for b in zeros:
            assert b["workerNeed"] == 0 and b["workerNeed"] is not None, \
                f"workerNeed==0 的建筑必须上报 0 而不是缺失/null：{b}"


# ===========================================================================
#  H. shutdown
# ===========================================================================
class TestH_Shutdown:
    def test_shutdown_exits_zero(self, spawn):
        c = spawn()
        c.call("ping")
        resp = c.call("shutdown")
        assert R(resp)["ok"] is True, resp
        code = c.wait_exit(timeout=5.0)
        assert code == 0, f"shutdown 后进程未在 5s 内以 0 退出（实际 {code}）"


# ===========================================================================
#  I. 回归：CLI 未被破坏
# ===========================================================================
CLI_SCRIPT = "\n".join([
    "help",
    "status",
    "scan 3 3",
    "list",
    "tech",
    "build farm 5 4",
    "status",
    "", "", "",            # 空行推进周期
    "status",
    "detail",
    "log",
    "quit",
]) + "\n"


CURRENT_CLI = BUILD / "qa_tmp" / "starcolony_current"


def _ensure_current_cli():
    """从当前 src/ 编译一份 CLI，避免 build/starcolony 可能是旧产物。"""
    srcs = [SRC / n for n in ("main.cpp", "ui.cpp", "game.cpp", "game.hpp", "types.hpp", "ui.hpp")]
    newest = max((p.stat().st_mtime for p in srcs if p.exists()), default=0)
    if not CURRENT_CLI.exists() or CURRENT_CLI.stat().st_mtime < newest:
        try:
            subprocess.run(
                ["g++", "-std=c++20", "-O2", "-Wall", "-Wextra", "-Isrc",
                 "src/main.cpp", "src/ui.cpp", "src/game.cpp", "-o", str(CURRENT_CLI)],
                cwd=str(ROOT), check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            )
        except (OSError, subprocess.CalledProcessError) as ex:
            pytest.skip(f"无法从当前 src/ 编译 CLI：{ex}")
    return CURRENT_CLI


def _run_cli(binary, seed=20240101):
    env = os.environ.copy()
    env.setdefault("TMPDIR", str(BUILD / "tmp"))
    p = subprocess.run(
        [str(binary), "--seed", str(seed), "--no-color"],
        input=CLI_SCRIPT.encode("utf-8"),
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        cwd=str(ROOT), env=env, timeout=90,
    )
    return p.stdout


def test_I_cli_unchanged_vs_baseline():
    if not BASELINE_CLI.exists():
        pytest.skip(f"基线 CLI 未构建（需由 22af9bb 源码编译）：{BASELINE_CLI}")
    cur_bin = _ensure_current_cli()          # 用当前 src/ 重新编译，反映工程师最新改动
    cur = _run_cli(cur_bin)
    base = _run_cli(BASELINE_CLI)
    if cur != base:
        # 给出首个差异位置，便于定位
        i = next((k for k in range(min(len(cur), len(base))) if cur[k] != base[k]),
                 min(len(cur), len(base)))
        pytest.fail(
            f"CLI 输出与基线不一致：首个差异 @byte {i}\n"
            f"  cur  = {cur[max(0, i - 40):i + 40]!r}\n"
            f"  base = {base[max(0, i - 40):i + 40]!r}\n"
            f"  (cur {len(cur)} bytes, base {len(base)} bytes)"
        )
