#!/usr/bin/env python3
# 星际争霸：殖民地 (Star Colony) —— P3a 内容校验器「对抗性测试」（QA 严过关 / Edward）
#
# 目标（契约 docs/CONTENT.md §8）：坏内容必须被**拒绝**，且错误信息可定位、一次报全、失败不留半初始化。
# 这里专挑校验器最容易放水的地方，构造坏内容逐步逼迫它露出破绽：
#   ① 缺必填字段   ② 类型不符（整数字段给 90.0）   ③ 未知科技 key（拼写相近）
#   ④ 数组长度不符 ⑤ 科技成环 / 自引用            ⑥ 拼错字段名（应给「是否想写 X？」）
# 外加两条横切要求：**一次报全部错误**、**失败不动全局表（半初始化保护）**。
#
# 重要：
#   * 所有坏内容都构造在 build/tmp/ 的**临时副本**里，绝不改动 content/base 原件。
#   * 不依赖 os.unlink（沙箱会把删除重定向到不可写的 ~/.Trash-0 → 假红）；用「覆盖写」建新副本。
#
# 运行：
#   export TMPDIR=/home/shc/starcolony/build/tmp
#   .venv/bin/python -m pytest frontend/tests/content_validator_adversarial.py -v
from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
CONTENT = ROOT / "content" / "base"
QA_TMP = ROOT / "build" / "qa_tmp"
WORK = ROOT / "build" / "tmp" / "qa_valadv"
PROBE_SRC = Path(__file__).resolve().parent / "qa_content_probe.cpp"
PROBE_BIN = QA_TMP / "qa_val_probe"

FILES = ("buildings.json", "techs.json", "weathers.json", "tuning.json", "manifest.json")


# ----------------------------- 基础设施 -----------------------------
def _newest(paths) -> float:
    return max((p.stat().st_mtime for p in paths if p.exists()), default=0.0)


def _ensure_probe() -> Path:
    """把校验探针编译到 build/qa_tmp/（带 mtime 缓存；content.cpp 含 json 头，较慢，只编一次）。"""
    QA_TMP.mkdir(parents=True, exist_ok=True)
    srcs = [PROBE_SRC, SRC / "content.cpp", SRC / "game.cpp",
            SRC / "content.hpp", SRC / "game.hpp", SRC / "types.hpp"]
    if PROBE_BIN.exists() and PROBE_BIN.stat().st_mtime >= _newest(srcs):
        return PROBE_BIN
    cmd = ["g++", "-std=c++20", "-O2", "-Wall", "-Wextra", "-Isrc", "-Ithird_party",
           str(PROBE_SRC), str(SRC / "content.cpp"), str(SRC / "game.cpp"), "-o", str(PROBE_BIN)]
    subprocess.run(cmd, cwd=str(ROOT), check=True,
                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return PROBE_BIN


def _run(mode: str, *dirs: str):
    """调用探针；返回 (returncode, stdout)。"""
    probe = _ensure_probe()
    p = subprocess.run([str(probe), mode, *[str(d) for d in dirs]],
                       cwd=str(ROOT), stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True, timeout=120)
    return p.returncode, p.stdout


def _err_lines(out: str):
    return [ln[4:] for ln in out.splitlines() if ln.startswith("ERR ")]


def _write_pack(d: Path, mutate) -> Path:
    """把一个（可改坏的）内容包写到目录 d：复制 content/base 全部文件，再让
    mutate(dict_of_name->obj) 就地修改。用「覆盖写」而非删除，规避沙箱对 os.unlink 的劫持。"""
    d.mkdir(parents=True, exist_ok=True)
    data = {fn: json.loads((CONTENT / fn).read_text(encoding="utf-8")) for fn in FILES}
    mutate(data)
    for fn, obj in data.items():
        (d / fn).write_text(json.dumps(obj, ensure_ascii=False, indent=2), encoding="utf-8")
    return d


def _make_pack(name: str, mutate) -> Path:
    """把改坏的内容包写到 WORK/<name>。"""
    return _write_pack(WORK / name, mutate)


@pytest.fixture(autouse=True)
def _tmpdir_env():
    os.environ.setdefault("TMPDIR", str(ROOT / "build" / "tmp"))
    yield


# ----------------------------- 基线正确性 -----------------------------
def test_valid_pack_loads_clean():
    """先证明：未改动的副本 → 0 错误、成功。否则后面的对抗测试都无意义。"""
    d = _make_pack("valid", lambda _data: None)
    rc, out = _run("load", d)
    assert rc == 0, out
    assert "RESULT OK" in out and "ERROR_COUNT 0" in out, out


# ----------------------------- 六类坏内容 -----------------------------
def test_missing_required_field():
    """① 缺必填字段 → 拒绝 + 文件+JSON路径+字段名。"""
    def mut(d):
        del d["buildings.json"][3]["cost"]["energy"]
    d = _make_pack("missing_field", mut)
    rc, out = _run("load", d)
    assert rc != 0, out
    errs = _err_lines(out)
    assert any("buildings.json" in e and "cost.energy" in e and "缺少必填字段" in e for e in errs), out


def test_type_mismatch_integer_given_float():
    """② 类型不符（整数字段给 90.0）→ 拒绝。"""
    def mut(d):
        d["buildings.json"][1]["build_turns"] = 2.0
        d["tuning.json"]["maxTurns"] = 90.0
    d = _make_pack("type_mismatch", mut)
    rc, out = _run("load", d)
    assert rc != 0, out
    errs = _err_lines(out)
    assert any("build_turns" in e and "类型不符" in e for e in errs), out
    assert any("maxTurns" in e and "类型不符" in e for e in errs), out


def test_unknown_tech_key_with_hint():
    """③ 未知科技 key（拼写相近）→ 拒绝 + 「是否想写 X？」提示。"""
    def mut(d):
        d["techs.json"][6]["requires"] = ["atmoo"]
    d = _make_pack("unknown_tech", mut)
    rc, out = _run("load", d)
    assert rc != 0, out
    errs = _err_lines(out)
    hit = [e for e in errs if "未知科技 key" in e and "atmoo" in e]
    assert hit, out
    assert any("atmo" in e and ("是否想写" in e or "did you mean" in e.lower()) for e in hit), out


def test_array_length_mismatch():
    """④ 数组长度不符（天气 5→4）→ 拒绝。"""
    def mut(d):
        d["weathers.json"].pop()
    d = _make_pack("array_len", mut)
    rc, out = _run("load", d)
    assert rc != 0, out
    assert any("weathers.json" in e and ("数组长度" in e or "!= 期望" in e) for e in _err_lines(out)), out


def test_tech_cycle_rejected():
    """⑤ 科技成环 / 自引用 → 拒绝，且报告可定位到具体科技。"""
    def mut(d):
        d["techs.json"][0]["requires"] = ["gate"]     # 直接成环
        d["techs.json"][2]["requires"] = ["fusion"]   # 自引用
    d = _make_pack("tech_cycle", mut)
    rc, out = _run("load", d)
    assert rc != 0, out
    errs = _err_lines(out)
    assert any(("成环" in e or "自引用" in e) for e in errs), out
    assert any("techs.json" in e for e in errs), out


def test_misspelled_field_with_hint():
    """⑥ 拼错字段名 → 拒绝 + 「是否想写 X？」。"""
    def mut(d):
        d["tuning.json"]["foodPerpop"] = d["tuning.json"].pop("foodPerPop")
    d = _make_pack("typo_field", mut)
    rc, out = _run("load", d)
    assert rc != 0, out
    errs = _err_lines(out)
    hit = [e for e in errs if "foodPerpop" in e and "未知字段" in e]
    assert hit, out
    assert any("foodPerPop" in e and ("是否想写" in e or "did you mean" in e.lower()) for e in hit), out


# ----------------------------- 横切要求 -----------------------------
def test_reports_all_errors_at_once():
    """一次性报告全部错误：构造多文件多处错误，断言一次全报（不是只报第一条）。"""
    def mut(d):
        del d["buildings.json"][3]["cost"]["energy"]          # buildings 缺字段
        d["techs.json"][6]["requires"] = ["atmoo"]            # techs 未知 key
        d["weathers.json"].pop()                              # weathers 长度
        d["tuning.json"]["nopeField"] = 1                     # tuning 未知字段
        del d["tuning.json"]["startMetal"]                    # tuning 缺字段
    d = _make_pack("many_errors", mut)
    rc, out = _run("load", d)
    assert rc != 0, out
    errs = _err_lines(out)
    # 五个文件里至少覆盖 4 个不同的错误来源文件
    files = {"buildings.json", "techs.json", "weathers.json", "tuning.json"}
    hit_files = {f for f in files if any(f in e for e in errs)}
    assert len(hit_files) >= 4, f"未一次报全：命中文件={hit_files}\n{out}"
    assert len(errs) >= 5, f"错误条数偏少：{len(errs)}\n{out}"


def test_failed_load_leaves_globals_untouched():
    """半初始化保护：先成功加载一份好内容，再用坏内容覆盖加载，全局表必须原封不动。"""
    good = _make_pack("halfinit_good", lambda _d: None)
    # 用「缺必填字段」确保坏内容一定 FAIL
    bad = _make_pack("halfinit_bad", lambda d: d["buildings.json"][3]["cost"].pop("energy"))
    rc, out = _run("halfinit", good, bad)
    assert "SECOND_RESULT FAIL" in out, out
    assert "GLOBALS_UNCHANGED YES" in out, out


# ----------------------------- 启动路径（真实二进制） -----------------------------
CLI_BIN = QA_TMP / "starcolony_startup"


def _ensure_cli() -> Path:
    """把真实 CLI 编到 build/qa_tmp/（mtime 缓存）——用于测「启动时是否拒绝」。"""
    QA_TMP.mkdir(parents=True, exist_ok=True)
    srcs = [SRC / "main.cpp", SRC / "ui.cpp", SRC / "game.cpp", SRC / "content.cpp",
            SRC / "game.hpp", SRC / "ui.hpp", SRC / "types.hpp", SRC / "content.hpp"]
    if CLI_BIN.exists() and CLI_BIN.stat().st_mtime >= _newest(srcs):
        return CLI_BIN
    cmd = ["g++", "-std=c++20", "-O2", "-Wall", "-Wextra", "-Isrc", "-Ithird_party",
           str(SRC / "main.cpp"), str(SRC / "ui.cpp"), str(SRC / "game.cpp"),
           str(SRC / "content.cpp"), "-o", str(CLI_BIN)]
    subprocess.run(cmd, cwd=str(ROOT), check=True,
                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return CLI_BIN


def test_startup_never_silently_falls_back_on_explicit_dir():
    """契约 §2：SC_CONTENT_DIR 指向「存在但校验失败」的包 → 引擎必须拒绝启动并报可定位错误。

    这条测真实二进制的**启动路径**（比只测 validator 函数更外层）。历史：曾存在
    「显式坏目录被静默回退到 exe 同级 content/base」的真 bug；现源码以
    `!dirExplicitRef()` 门控候选回退，已修复。本用例即其回归护栏。
    """
    bad = _make_pack("startup_bad", lambda d: d["buildings.json"][3]["cost"].pop("energy"))
    cli = _ensure_cli()
    env = os.environ.copy()
    env["SC_CONTENT_DIR"] = str(bad)
    p = subprocess.run([str(cli), "--seed", "1", "--no-color"], cwd=str(ROOT), env=env,
                       input=b"quit\n", stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       timeout=60)
    combined = (p.stdout + p.stderr).decode("utf-8", "replace")
    assert p.returncode != 0, f"坏内容未拒绝启动（exit={p.returncode}）\n{combined[:400]}"
    assert str(bad) in combined or "内容加载失败" in combined, combined[:400]


def test_startup_explicit_missing_dir_rejected():
    """显式指向不存在目录 → 也必须拒绝启动（不静默回退）。"""
    cli = _ensure_cli()
    env = os.environ.copy()
    env["SC_CONTENT_DIR"] = str(WORK / "definitely_absent_dir_xyz")
    p = subprocess.run([str(cli), "--seed", "1", "--no-color"], cwd=str(ROOT), env=env,
                       input=b"quit\n", stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       timeout=60)
    assert p.returncode != 0, (p.stdout + p.stderr).decode("utf-8", "replace")[:400]


def test_default_dir_still_uses_exe_candidates_without_env():
    """反向锁定：**未设** SC_CONTENT_DIR 时，exe 同级候选回退必须仍然工作
    （从任意工作目录启动都能找到 content/base）——别为了修上面的 bug 把便利路径打死。"""
    empty_cwd = WORK / "empty_cwd"
    empty_cwd.mkdir(parents=True, exist_ok=True)
    cli = _ensure_cli()
    env = os.environ.copy()
    env.pop("SC_CONTENT_DIR", None)
    p = subprocess.run([str(cli), "--seed", "1", "--no-color"], cwd=str(empty_cwd), env=env,
                       input=b"quit\n", stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       timeout=60)
    combined = (p.stdout + p.stderr).decode("utf-8", "replace")
    assert p.returncode == 0, combined[:400]
    assert "STAR COLONY" in combined, combined[:400]
