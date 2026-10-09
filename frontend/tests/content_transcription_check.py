#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# 星际争霸：殖民地 (Star Colony) —— P3a「内容外置」跨语言独立转写核对（QA / Edward）
#
# 为什么单独成文件（而不是并入 p1_baseline_regression.py）：
#   p1_baseline_regression 管的是「**行为**是否零变化」（编译后跑二进制、比字节），
#   本文件管的是「**数值转写**是否正确」（纯 Python 解析 JSON ↔ 解析 git 历史源码）。两者
#   失败模式、是否依赖编译、被谁触发都不同：内容改一个字就可能让本文件单独变红，而行为
#   基线却未必动。因此分开，既可 `pytest` 跑，也可当独立脚本 `python <本文件> --selftest` 用。
#
# 权威依据：docs/CONTENT.md（P3a 冻结）。对照物是 **git 22af9bb** 的源码（改造前），
# 而不是当前 src/ —— 当前 src/ 正在被改造成读 JSON，不能拿来当"原始值"的真相来源。
#
# 运行：
#   export TMPDIR=/home/shc/starcolony/build/tmp
#   .venv/bin/python -m pytest frontend/tests/content_transcription_check.py -v
#   自验（不需要真内容，注入转写错误确认比对脚本能抓到）：
#   .venv/bin/python frontend/tests/content_transcription_check.py --selftest
from __future__ import annotations

import decimal
import json
import re
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
CONTENT = ROOT / "content" / "base"
REF_COMMIT = "22af9bb"
D = decimal.Decimal

# 契约 §4：color 用名字而非整数，顺序与 Col 枚举一致
COLOR_NAMES = ["default", "grey", "red", "green", "yellow",
               "blue", "magenta", "cyan", "white", "bright_white"]


# ---------------------------------------------------------------------------
#  从 git 历史取「改造前」源码（对照物）
# ---------------------------------------------------------------------------
def git_show(path: str, commit: str = REF_COMMIT) -> str:
    p = subprocess.run(["git", "show", f"{commit}:{path}"],
                       cwd=str(ROOT), capture_output=True, text=True)
    assert p.returncode == 0, f"git show {commit}:{path} 失败：{p.stderr.strip()}"
    return p.stdout


# ---------------------------------------------------------------------------
#  源码解析器（全部作用在**传入的文本**上，因此可指向任意版本）
# ---------------------------------------------------------------------------
def parse_consts(text: str) -> dict:
    """types.hpp 里的 inline constexpr int 常量（MAX_TURNS 等）。"""
    out = {}
    for m in re.finditer(r"inline constexpr int\s+(\w+)\s*=\s*(-?\d+)", text):
        out[m.group(1)] = int(m.group(2))
    return out


def parse_col_enum(text: str) -> dict:
    """COL_XXX -> 序号。"""
    block = re.search(r"enum Col\s*:\s*int\s*\{(.*?)\}", text, re.S).group(1)
    vals, idx = {}, 0
    for m in re.finditer(r"(COL_\w+)\s*(?:=\s*(-?\d+))?", block):
        if m.group(2) is not None:
            idx = int(m.group(2))
        vals[m.group(1)] = idx
        idx += 1
    assert vals.get("COL_DEF") == 0 and vals.get("COL_BWHITE") == 9, vals
    return vals


def parse_tech_enum(text: str) -> list:
    """Tech 枚举名（声明顺序，不含 COUNT）。"""
    block = re.search(r"enum class Tech\s*:\s*int\s*\{(.*?)\}", text, re.S).group(1)
    names = []
    for m in re.finditer(r"\b([A-Za-z_]\w*)\s*(?:=\s*\d+)?\s*,", block):
        if m.group(1) == "COUNT":
            break
        names.append(m.group(1))
    return names


def parse_weather_enum(text: str) -> list:
    block = re.search(r"enum class Weather\s*:\s*int\s*\{(.*?)\}", text, re.S).group(1)
    names = []
    for m in re.finditer(r"\b([A-Za-z_]\w*)\s*(?:=\s*\d+)?\s*,", block):
        if m.group(1) == "COUNT":
            break
        names.append(m.group(1))
    return names


def snake(name: str) -> str:
    return re.sub(r"(?<!^)(?=[A-Z])", "_", name).lower()


_BDEF_RE = re.compile(
    r"""\{\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*'(.)'\s*,\s*(COL_\w+)\s*,"""
    r"""\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,"""
    r"""\s*(true|false)\s*,\s*"([^"]*)"\s*\}"""
)


def parse_bdef(game_cpp: str) -> list:
    block = game_cpp.split("BDEF = {{", 1)[1].split("}};", 1)[0]
    rows = []
    for m in _BDEF_RE.finditer(block):
        k, name, glyph, col, M, E, Sc, bt, wk, up, rep, desc = m.groups()
        rows.append(dict(key=k, name=name, glyph=glyph, color=col,
                         costMetal=int(M), costEnergy=int(E), costScience=int(Sc),
                         buildTurns=int(bt), workers=int(wk), upkeep=int(up),
                         repeatable=(rep == "true"), desc=desc))
    return rows


_TDEF_RE = re.compile(
    r'\{\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*(-?\d+)\s*,\s*([^,]+?)\s*,\s*"([^"]*)"\s*\}'
)


def parse_tdef(game_cpp: str, tech_names: list) -> list:
    """TDEF 行；req 位掩码 → 前置科技 **key** 集合（按 Tech 枚举序 == TDEF 序映射）。"""
    block = game_cpp.split("TDEF = {{", 1)[1].split("}};", 1)[0]
    raw = []
    for m in _TDEF_RE.finditer(block):
        k, name, cost, req, desc = m.groups()
        raw.append((k, name, int(cost), req, desc))
    name_to_key = {tech_names[i]: raw[i][0] for i in range(min(len(tech_names), len(raw)))}
    rows = []
    for k, name, cost, req, desc in raw:
        bits = [name_to_key[e] for e in re.findall(r"Tech::(\w+)", req)]
        rows.append(dict(key=k, name=name, cost=cost, requires=bits, desc=desc))
    return rows


_WDEF_RE = re.compile(
    r'\{\s*"([^"]*)"\s*,\s*(COL_\w+)\s*,\s*([\d.]+)\s*,\s*([\d.]+)\s*,'
    r"\s*([\d.]+)\s*,\s*([\d.]+)\s*,\s*\"([^\"]*)\"\s*\}"
)


def parse_wdef(game_cpp: str) -> list:
    block = game_cpp.split("WDEF = {{", 1)[1].split("}};", 1)[0]
    rows = []
    for m in _WDEF_RE.finditer(block):
        name, col, metal, energy, food, science, desc = m.groups()
        rows.append(dict(name=name, color=col, metal=D(metal), energy=D(energy),
                         food=D(food), science=D(science), desc=desc))
    return rows


def parse_tuning(types_hpp: str) -> list:
    """Tuning 字段：[(声明类型, 字段名, 字面量原文)]，字面量已解析常量名。"""
    consts = parse_consts(types_hpp)
    body = re.search(r"struct Tuning\s*\{(.*?)\n\};", types_hpp, re.S).group(1)
    body = re.sub(r"//[^\n]*", "", body)          # 去行注释
    fields = []
    for line in body.splitlines():
        line = line.strip()
        if not line:
            continue
        m = re.match(r"(int|double|float|bool)\s+(\w+)\s*=\s*([^;]+);", line)
        assert m, f"Tuning 有无法解析的行：{line!r}"
        typ, name, lit = m.group(1), m.group(2), m.group(3).strip()
        if lit in consts:
            lit = str(consts[lit])
        fields.append((typ, name, lit))
    return fields


# ---------------------------------------------------------------------------
#  参考模型（来自 22af9bb）
# ---------------------------------------------------------------------------
def reference() -> dict:
    types = git_show("src/types.hpp")
    game = git_show("src/game.cpp")
    return dict(
        consts=parse_consts(types),
        col=parse_col_enum(types),
        tech_names=parse_tech_enum(types),
        weather_names=parse_weather_enum(types),
        bdef=parse_bdef(game),
        tdef=parse_tdef(game, parse_tech_enum(types)),
        wdef=parse_wdef(game),
        tuning=parse_tuning(types),
    )


# ---------------------------------------------------------------------------
#  比对：返回 (errors, notes)。errors 非空即转写有问题。
# ---------------------------------------------------------------------------
def _load(path: Path):
    return json.loads(path.read_text(encoding="utf-8"), parse_float=D)


def _err(errors, path, msg):
    errors.append(f"{path}: {msg}")


def check_pack(pack_dir: Path, ref: dict | None = None):
    ref = ref or reference()
    errors: list[str] = []
    notes: list[str] = []
    col_name = {v: COLOR_NAMES[v] for v in ref["col"].values()}

    # ---------------- buildings ----------------
    bpath = pack_dir / "buildings.json"
    if not bpath.exists():
        errors.append("buildings.json: 文件缺失")
    else:
        got = _load(bpath)
        exp = ref["bdef"]
        if not isinstance(got, list):
            errors.append("buildings.json: 顶层必须是数组")
        else:
            if len(got) != len(exp):
                errors.append(f"buildings.json: 数组长度 {len(got)} != 期望 {len(exp)}"
                              f"（BTYPE_COUNT）")
            for i, row in enumerate(exp):
                if i >= len(got):
                    break
                g = got[i]
                p = f"buildings.json[{i}]"
                if not isinstance(g, dict):
                    errors.append(f"{p}: 元素必须是对象"); continue
                checks = [
                    ("key", g.get("key"), row["key"]),
                    ("name", g.get("name"), row["name"]),
                    ("build_turns", g.get("build_turns"), row["buildTurns"]),
                    ("workers", g.get("workers"), row["workers"]),
                    ("upkeep", g.get("upkeep"), row["upkeep"]),
                    ("repeatable", g.get("repeatable"), row["repeatable"]),
                    ("desc", g.get("desc"), row["desc"]),
                ]
                for field, gv, ev in checks:
                    if gv is None:
                        _err(errors, p, f"缺少必填字段 {field!r}")
                    elif gv != ev:
                        _err(errors, p, f"{field}: 期望 {ev!r}，实际 {gv!r}")
                # glyph 单字符
                gv = g.get("glyph")
                if gv is None:
                    _err(errors, p, "缺少必填字段 'glyph'")
                elif not (isinstance(gv, str) and len(gv) == 1):
                    _err(errors, p, f"glyph 必须是单字符，实际 {gv!r}")
                elif gv != row["glyph"]:
                    _err(errors, p, f"glyph: 期望 {row['glyph']!r}，实际 {gv!r}")
                # color 名字
                gv = g.get("color")
                want_color = col_name[ref["col"][row["color"]]]
                if gv is None:
                    _err(errors, p, "缺少必填字段 'color'")
                elif gv != want_color:
                    _err(errors, p, f"color: 期望 {want_color!r}，实际 {gv!r}")
                # cost 三项
                cost = g.get("cost")
                want_cost = {"metal": row["costMetal"], "energy": row["costEnergy"],
                             "science": row["costScience"]}
                if not isinstance(cost, dict):
                    _err(errors, p, "缺少必填字段 'cost'（对象）")
                else:
                    for k, ev in want_cost.items():
                        if k not in cost:
                            _err(errors, f"{p}.cost", f"缺少必填字段 {k!r}")
                        elif cost[k] != ev:
                            _err(errors, f"{p}.cost", f"{k}: 期望 {ev}，实际 {cost[k]!r}")
            # 唯一性 / hq 约束
            keys = [g.get("key") for g in got if isinstance(g, dict)]
            glyphs = [g.get("glyph") for g in got if isinstance(g, dict)]
            for name, seq in (("key", keys), ("glyph", glyphs)):
                dup = {x for x in seq if seq.count(x) > 1}
                if dup:
                    _err(errors, "buildings.json", f"{name} 必须唯一，重复：{sorted(map(str, dup))}")
            hq = next((g for g in got if isinstance(g, dict) and g.get("key") == "hq"), None)
            if hq is None:
                _err(errors, "buildings.json", "必须存在 'hq'")
            elif hq.get("repeatable") is not False:
                _err(errors, "buildings.json", "hq 必须 repeatable=false")

    # ---------------- techs ----------------
    tpath = pack_dir / "techs.json"
    if not tpath.exists():
        errors.append("techs.json: 文件缺失")
    else:
        got = _load(tpath)
        exp = ref["tdef"]
        if not isinstance(got, list):
            errors.append("techs.json: 顶层必须是数组")
        else:
            if len(got) != len(exp):
                errors.append(f"techs.json: 数组长度 {len(got)} != 期望 {len(exp)}"
                              f"（TECH_COUNT）")
            for i, row in enumerate(exp):
                if i >= len(got):
                    break
                g = got[i]
                p = f"techs.json[{i}]"
                if not isinstance(g, dict):
                    errors.append(f"{p}: 元素必须是对象"); continue
                for field, gv, ev in (("key", g.get("key"), row["key"]),
                                      ("name", g.get("name"), row["name"]),
                                      ("cost", g.get("cost"), row["cost"]),
                                      ("desc", g.get("desc"), row["desc"])):
                    if gv is None:
                        _err(errors, p, f"缺少必填字段 {field!r}")
                    elif gv != ev:
                        _err(errors, p, f"{field}: 期望 {ev!r}，实际 {gv!r}")
                req = g.get("requires")
                if not isinstance(req, list):
                    _err(errors, p, "缺少必填字段 'requires'（数组）")
                elif set(req) != set(row["requires"]):
                    _err(errors, p, f"requires: 期望 {sorted(row['requires'])}，"
                                    f"实际 {sorted(map(str, req))}")

    # ---------------- weathers ----------------
    wpath = pack_dir / "weathers.json"
    if not wpath.exists():
        errors.append("weathers.json: 文件缺失")
    else:
        got = _load(wpath)
        exp = ref["wdef"]
        if not isinstance(got, list):
            errors.append("weathers.json: 顶层必须是数组")
        else:
            if len(got) != len(exp):
                errors.append(f"weathers.json: 数组长度 {len(got)} != 期望 {len(exp)}"
                              f"（WEATHER_COUNT）")
            for i, row in enumerate(exp):
                if i >= len(got):
                    break
                g = got[i]
                p = f"weathers.json[{i}]"
                if not isinstance(g, dict):
                    errors.append(f"{p}: 元素必须是对象"); continue
                want_color = col_name[ref["col"][row["color"]]]
                for field, gv, ev in (("name", g.get("name"), row["name"]),
                                      ("color", g.get("color"), want_color),
                                      ("desc", g.get("desc"), row["desc"])):
                    if gv is None:
                        _err(errors, p, f"缺少必填字段 {field!r}")
                    elif gv != ev:
                        _err(errors, p, f"{field}: 期望 {ev!r}，实际 {gv!r}")
                mult = g.get("mult")
                want = {"metal": row["metal"], "energy": row["energy"],
                        "food": row["food"], "science": row["science"]}
                if not isinstance(mult, dict):
                    _err(errors, p, "缺少必填字段 'mult'（对象）")
                else:
                    for k, ev in want.items():
                        if k not in mult:
                            _err(errors, f"{p}.mult", f"缺少必填字段 {k!r}")
                        else:
                            gv = mult[k]
                            if not isinstance(gv, (int, D)):
                                _err(errors, f"{p}.mult", f"{k}: 必须是数字，实际 {gv!r}")
                            elif D(gv) != ev:
                                _err(errors, f"{p}.mult",
                                     f"{k}: 期望 {ev}，实际 {gv!r}（精确值比较）")
            # key 是新增字段：契约只强制第一个是 clear + 唯一
            wkeys = [g.get("key") for g in got if isinstance(g, dict)]
            if wkeys and wkeys[0] != "clear":
                _err(errors, "weathers.json", f"第一个天气的 key 必须是 'clear'，实际 {wkeys[0]!r}")
            if any(k is None for k in wkeys):
                _err(errors, "weathers.json", "每个天气都必须有 'key'")
            else:
                dup = {k for k in wkeys if wkeys.count(k) > 1}
                if dup:
                    _err(errors, "weathers.json", f"key 必须唯一，重复：{sorted(dup)}")
                want_keys = [snake(n) for n in ref["weather_names"]]
                if wkeys != want_keys:
                    notes.append(f"天气 key 与枚举名 snake_case 不一致：{wkeys} vs {want_keys}"
                                 "（契约未强制，仅供参考）")

    # ---------------- tuning ----------------
    upath = pack_dir / "tuning.json"
    if not upath.exists():
        errors.append("tuning.json: 文件缺失")
    else:
        got = _load(upath)
        exp = ref["tuning"]
        if not isinstance(got, dict):
            errors.append("tuning.json: 顶层必须是对象")
        else:
            expected_names = {n for _, n, _ in exp}
            for extra in sorted(set(got) - expected_names):
                errors.append(f'tuning.json: unknown field "{extra}"'
                              f"（Tuning 里没有这个字段）")
            for typ, name, lit in exp:
                if name not in got:
                    errors.append(f"tuning.json: {name}: 缺少必填字段")
                    continue
                gv = got[name]
                if typ == "int":
                    if isinstance(gv, bool) or not isinstance(gv, int):
                        errors.append(f"tuning.json: {name}: 必须是整数，实际 {gv!r}"
                                      "（整数字段给浮点算失败，如 90.0）")
                    elif gv != int(D(lit)):
                        errors.append(f"tuning.json: {name}: 期望 {int(D(lit))}，实际 {gv}")
                elif typ in ("double", "float"):
                    if isinstance(gv, bool) or not isinstance(gv, (int, D)):
                        errors.append(f"tuning.json: {name}: 必须是数字，实际 {gv!r}")
                    elif D(gv) != D(lit):
                        errors.append(f"tuning.json: {name}: 期望 {lit}，实际 {gv}"
                                      "（精确值比较：1.15 不能被写成 1.1）")
                elif typ == "bool":
                    if not isinstance(gv, bool):
                        errors.append(f"tuning.json: {name}: 必须是布尔，实际 {gv!r}")
                    elif gv != (lit == "true"):
                        errors.append(f"tuning.json: {name}: 期望 {lit}，实际 {gv}")

    return errors, notes


# ---------------------------------------------------------------------------
#  科技树：无环 / 引用存在 / 无自引用（自己算，不依赖引擎校验器）
# ---------------------------------------------------------------------------
def tech_graph_errors(techs: list) -> list:
    errors = []
    keys = [t.get("key") for t in techs]
    for i, t in enumerate(techs):
        req = t.get("requires")
        if req is None:
            continue
        if t.get("key") in req:
            errors.append(f"techs.json[{i}]: 科技 {t.get('key')!r} 不能依赖自身")
        for r in req:
            if r not in keys:
                errors.append(f"techs.json[{i}].requires: 未知科技 key {r!r}")
    # DFS 找环
    graph = {t.get("key"): [r for r in (t.get("requires") or []) if r in keys] for t in techs}
    WHITE, GRAY, BLACK = 0, 1, 2
    color = {k: WHITE for k in graph}
    stack = []

    def dfs(u):
        color[u] = GRAY
        stack.append(u)
        for v in graph.get(u, []):
            if color.get(v) == GRAY:
                cyc = stack[stack.index(v):] + [v]
                errors.append(f"techs.json: 科技树成环：{' -> '.join(cyc)}")
            elif color.get(v) == WHITE:
                dfs(v)
        stack.pop()
        color[u] = BLACK

    for k in graph:
        if color[k] == WHITE:
            dfs(k)
    return errors


# ---------------------------------------------------------------------------
#  生成"正确"内容包（仅供自验：用来注入错误、验证比对脚本有牙齿）
# ---------------------------------------------------------------------------
def reference_pack(ref: dict | None = None) -> dict:
    ref = ref or reference()
    col_name = {v: COLOR_NAMES[v] for v in ref["col"].values()}
    buildings = [{
        "key": r["key"], "name": r["name"], "glyph": r["glyph"],
        "color": col_name[ref["col"][r["color"]]],
        "cost": {"metal": r["costMetal"], "energy": r["costEnergy"], "science": r["costScience"]},
        "build_turns": r["buildTurns"], "workers": r["workers"], "upkeep": r["upkeep"],
        "repeatable": r["repeatable"], "desc": r["desc"],
    } for r in ref["bdef"]]
    techs = [{"key": r["key"], "name": r["name"], "cost": r["cost"],
              "requires": list(r["requires"]), "desc": r["desc"]} for r in ref["tdef"]]
    weathers = [{
        "key": snake(n), "name": r["name"], "color": col_name[ref["col"][r["color"]]],
        "mult": {"metal": float(r["metal"]), "energy": float(r["energy"]),
                 "food": float(r["food"]), "science": float(r["science"])},
        "desc": r["desc"],
    } for n, r in zip(ref["weather_names"], ref["wdef"])]
    tuning = {}
    for typ, name, lit in ref["tuning"]:
        if typ == "int":
            tuning[name] = int(D(lit))
        elif typ == "bool":
            tuning[name] = (lit == "true")
        else:
            tuning[name] = float(D(lit))
    manifest = {"schema": 1, "pack_id": "base", "name": "基础内容包",
                "files": {"buildings": "buildings.json", "techs": "techs.json",
                          "weathers": "weathers.json", "tuning": "tuning.json"}}
    return {"manifest.json": manifest, "buildings.json": buildings,
            "techs.json": techs, "weathers.json": weathers, "tuning.json": tuning}


def write_pack(pack_dir: Path, pack: dict):
    pack_dir.mkdir(parents=True, exist_ok=True)
    for name, obj in pack.items():
        (pack_dir / name).write_text(
            json.dumps(obj, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def _mutations(pack: dict):
    """(说明, 期望错误里出现的子串, 变异函数)。"""
    def mut(fn):
        p = json.loads(json.dumps(pack, ensure_ascii=False))
        fn(p)
        return p

    return [
        ("desc 少一个字符", "desc: 期望",
         mut(lambda p: p["buildings.json"][0].update(desc=p["buildings.json"][0]["desc"][:-1]))),
        ("浮点 1.15 写成 1.1", "foodPerPop: 期望 1.15",
         mut(lambda p: p["tuning.json"].update(foodPerPop=1.1))),
        ("color 名称错", "color: 期望 'yellow'",
         mut(lambda p: p["buildings.json"][1].update(color="red"))),
        ("glyph 两个字符", "glyph 必须是单字符",
         mut(lambda p: p["buildings.json"][2].update(glyph="GG"))),
        ("glyph 重复", "glyph 必须唯一",
         mut(lambda p: p["buildings.json"][1].update(glyph="C"))),
        ("requires 漏一项", "requires: 期望",
         mut(lambda p: p["techs.json"][7].update(requires=["fusion"]))),
        ("requires 未知 key", "未知科技 key",
         mut(lambda p: p["techs.json"][3].update(requires=["atmoo"]))),
        ("科技自引用", "不能依赖自身",
         mut(lambda p: p["techs.json"][2].update(requires=["fusion"]))),
        ("科技成环", "科技树成环",
         mut(lambda p: p["techs.json"][0].update(requires=["gate"]))),
        ("tuning 字段名拼错", 'unknown field "foodPerpop"',
         mut(lambda p: (p["tuning.json"].update(foodPerpop=p["tuning.json"].pop("foodPerPop"))))),
        ("tuning 整数字段给浮点", "必须是整数",
         mut(lambda p: p["tuning.json"].update(maxTurns=90.0))),
        ("tuning 缺字段", "startMetal: 缺少必填字段",
         mut(lambda p: p["tuning.json"].pop("startMetal"))),
        ("buildings 数组短一条", "数组长度",
         mut(lambda p: p["buildings.json"].pop())),
        ("cost 缺 energy", "cost: 缺少必填字段 'energy'",
         mut(lambda p: p["buildings.json"][3]["cost"].pop("energy"))),
        ("weather 倍率错", "energy: 期望 0.50",
         mut(lambda p: p["weathers.json"][1]["mult"].update(energy=0.9))),
    ]


def selftest() -> int:
    print("=" * 72)
    print("内容转写比对脚本 —— 自验（注入转写错误，必须被精准抓到）")
    print("=" * 72)
    ref = reference()
    work = ROOT / "build" / "tmp" / "qa_content_selftest"
    pack = reference_pack(ref)

    print(f"\n[0] 对照物：git {REF_COMMIT}")
    print(f"    BDEF={len(ref['bdef'])}  TDEF={len(ref['tdef'])}  "
          f"WDEF={len(ref['wdef'])}  Tuning={len(ref['tuning'])} 字段")

    # 锚点：手工从源码抄录的少量真值，独立于解析器，防"解析器 + 生成器同源同错"
    b0 = ref["bdef"][0]
    assert (b0["key"], b0["name"], b0["glyph"], b0["color"]) == ("hq", "指挥中心", "C", "COL_BLUE"), b0
    assert b0["desc"] == "殖民地核心：+4 能源 +1 科研，提供 8 人口上限", b0["desc"]
    t7 = ref["tdef"][7]
    assert (t7["key"], t7["cost"], set(t7["requires"])) == ("gate", 170, {"fusion", "atmo"}), t7
    w1 = ref["wdef"][1]
    assert (w1["name"], w1["metal"], w1["energy"], w1["food"]) == ("沙暴", D("0.90"), D("0.50"), D("0.80")), w1
    tune = {n: lit for _, n, lit in ref["tuning"]}
    assert tune["foodPerPop"] == "1.15" and tune["maxTurns"] == "90", tune
    print("    ✓ 手工锚点（hq / gate / 沙暴 / foodPerPop）与源码一致")

    # 正确包 → 0 错误
    write_pack(work / "correct", pack)
    errs, _ = check_pack(work / "correct", ref)
    ok = not errs
    print(f"\n[1] 正确内容包 → 期望 0 错误：{'PASS' if ok else 'FAIL'}")
    for e in errs[:5]:
        print("    !", e)

    print("\n[2] 注入转写错误 → 必须逐条被抓")
    failures = [] if ok else ["正确包未能零错误"]
    for desc, want, mutated in _mutations(pack):
        write_pack(work / "mutated", mutated)
        errs, _ = check_pack(work / "mutated", ref)
        # 外加科技树自建校验（成环/未知 key/自引用由它负责）
        te = tech_graph_errors(mutated["techs.json"])
        blob = "\n".join(errs + te)
        caught = want in blob
        print(f"    {'OK ' if caught else 'MISS'} {desc:<22} 期望包含：{want!r}")
        if not caught:
            failures.append(f"{desc} 未被抓到（期望子串 {want!r}）")
            print("        实际：", blob[:300])

    print("\n" + "=" * 72)
    if failures:
        print("自验失败：")
        for f in failures:
            print("  ✗", f)
        return 1
    print("自验通过：正确包零错误，所有注入的转写错误都被精准抓到。")
    return 0


# ---------------------------------------------------------------------------
#  pytest 用例（真内容未落地时 skip，并说明原因）
# ---------------------------------------------------------------------------
def _has_real_content() -> bool:
    return all((CONTENT / n).exists() for n in
               ("manifest.json", "buildings.json", "techs.json", "weathers.json", "tuning.json"))


requires_content = pytest.mark.skipif(
    not _has_real_content(),
    reason=f"content/base 尚未落地（{CONTENT}）—— 转写比对需真实内容，运行 --selftest 可自验脚本本身")


@requires_content
def test_transcription_matches_22af9bb():
    ref = reference()
    errs, notes = check_pack(CONTENT, ref)
    for n in notes:
        print("note:", n)
    assert not errs, "内容转写与 22af9bb 不一致：\n" + "\n".join(errs)


@requires_content
def test_tech_tree_acyclic_and_resolvable():
    techs = json.loads((CONTENT / "techs.json").read_text(encoding="utf-8"))
    errs = tech_graph_errors(techs)
    assert not errs, "科技树校验失败：\n" + "\n".join(errs)


def test_reference_anchors():
    """解析器自身的手工锚点（防解析器写错导致比对假绿）。"""
    ref = reference()
    assert ref["bdef"][0]["desc"] == "殖民地核心：+4 能源 +1 科研，提供 8 人口上限"
    assert set(ref["tdef"][7]["requires"]) == {"fusion", "atmo"}
    assert [n for _, n, _ in ref["tuning"]].count("foodPerPop") == 1
    tune = {n: lit for _, n, lit in ref["tuning"]}
    assert tune["foodPerPop"] == "1.15"


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(selftest())
    print(__doc__)
    print("用法：python frontend/tests/content_transcription_check.py --selftest")
    sys.exit(0)
