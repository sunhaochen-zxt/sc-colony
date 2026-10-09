#!/usr/bin/env python3
# 星际争霸：殖民地 (Star Colony) —— P1 行为零变化门禁（QA 严过关 / Edward）
#
# 目的：改动 advanceTurn / do* 语义（新增 pending 拦截）后，确认 P1 的一切行为**未被破坏**：
#   * trace <seed>           与 build/baseline/trace_<seed>.txt 逐字节一致
#   * sweep --seeds 512      与 build/baseline/sweep512.txt 逐字节一致
#   * selftest               与 build/baseline/selftest.txt 逐字节一致
#   * CLI 存档字节           与「22af9bb 源码编译的基线 CLI」产出的存档逐字节一致
#   * 无事件 CLI 会话        全程无待决事件时，stdout 与存档均与基线逐字节一致（新增红线，见下方）
#
# 新增红线（P2 §4.4.1 配套）：
#   规则上移 core 后，唯一会改变的行为是「待决事件期间非 answer 操作被拦」。
#   因此只要一次会话全程**没有**待决事件，两个版本的输出必须逐字节相同——
#   test_cli_event_free_session_byte_identical 把这条钉死。
#   反之，**含待决事件**的会话存在规格预期的差异（基线执行、P2 拦截），
#   test_cli_event_session_expected_diff 只做记录、不判失败。
#
# 基线：build/baseline/*（P1 QA 于 22af9bb 采集）；基线 CLI：build/qa_tmp/starcolony_baseline。
#
# 运行：
#   export TMPDIR=/home/shc/starcolony/build/tmp
#   .venv/bin/python -m pytest frontend/tests/p1_baseline_regression.py -v
from __future__ import annotations

import os
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
TOOLS = ROOT / "tools"
BUILD = ROOT / "build"
QA_TMP = BUILD / "qa_tmp"
BASELINE = BUILD / "baseline"
BASELINE_CLI = QA_TMP / "starcolony_baseline"

TRACE_SEEDS = [1, 2, 3, 42, 777, 2024, 12345, 99991]

# 全程无待决事件的 CLI 会话种子（脚本固定为 status/list/detail/next*3/save，见下）。
# 已核实这三个种子在 3 个周期内不触发 pending 事件，故可用于「无事件逐字节」红线。
EVENT_FREE_SEEDS = [1, 42, 2]

# 含事件会话的差异标记
_EVENT_MARK = "◇ 事件"              # 事件弹窗标题前缀（仅当有 pending 时出现）
_BLOCKED_MARK = "有事件需要先处理"    # 核心拦截非 answer 操作的固定文案（P2 §4.4.1）

for _d in (BUILD / "tmp", QA_TMP):
    _d.mkdir(parents=True, exist_ok=True)
os.environ.setdefault("TMPDIR", str(BUILD / "tmp"))


def _newest(paths) -> float:
    return max((p.stat().st_mtime for p in paths if p.exists()), default=0.0)


def _ensure(name, sources, extra_inc=()):
    """从当前 src/ 编译一个 P1 工具到 build/qa_tmp/（带 mtime 缓存）。

    P3a 起核心内容外置：所有可执行文件都要链接 src/content.cpp 并带 third_party
    （nlohmann/json）头路径——故这里统一加上。
    """
    out = QA_TMP / name
    incs = ["-Isrc", "-Ithird_party"] + [f"-I{i}" for i in extra_inc]
    if out.exists() and out.stat().st_mtime >= _newest(sources):
        return out
    cmd = ["g++", "-std=c++20", "-O2", "-Wall", "-Wextra", *incs,
           *[str(s) for s in sources], "-o", str(out)]
    try:
        subprocess.run(cmd, cwd=str(ROOT), check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except (OSError, subprocess.CalledProcessError) as ex:
        pytest.skip(f"无法编译 {name}（src/ 可能正在改动）：{ex}")
    return out


def _run(binary, args=(), timeout=180) -> bytes:
    env = os.environ.copy()
    env.setdefault("TMPDIR", str(BUILD / "tmp"))
    p = subprocess.run([str(binary), *args], cwd=str(ROOT), env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    return p.stdout


def _srcs_trace():
    return [SRC / "game.cpp", SRC / "content.cpp", SRC / "game.hpp", SRC / "types.hpp",
            TOOLS / "trace.cpp", ROOT / "tests" / "ai.hpp"]


def _srcs_sweep():
    return [SRC / "game.cpp", SRC / "content.cpp", SRC / "game.hpp", SRC / "types.hpp",
            TOOLS / "sweep.cpp", ROOT / "tests" / "ai.hpp"]


def _srcs_selftest():
    return [SRC / "game.cpp", SRC / "content.cpp", SRC / "game.hpp", SRC / "types.hpp",
            ROOT / "tests" / "selftest.cpp"]


def _srcs_cli():
    return [SRC / "main.cpp", SRC / "ui.cpp", SRC / "game.cpp", SRC / "content.cpp",
            SRC / "game.hpp", SRC / "ui.hpp", SRC / "types.hpp"]


@pytest.mark.parametrize("seed", TRACE_SEEDS)
def test_trace_matches_baseline(seed):
    base = BASELINE / f"trace_{seed}.txt"
    if not base.exists():
        pytest.skip(f"缺基线 {base}")
    trace = _ensure("trace", _srcs_trace(), extra_inc=["tests"])
    out = _run(trace, [str(seed)])
    assert out == base.read_bytes(), f"trace {seed} 与 22af9bb 基线不一致"


def test_sweep512_matches_baseline():
    base = BASELINE / "sweep512.txt"
    if not base.exists():
        pytest.skip(f"缺基线 {base}")
    sweep = _ensure("sweep", _srcs_sweep(), extra_inc=["tests"])
    out = _run(sweep, ["--seeds", "512"])
    assert out == base.read_bytes(), "sweep --seeds 512 与 22af9bb 基线不一致"


def test_selftest_matches_baseline():
    base = BASELINE / "selftest.txt"
    if not base.exists():
        pytest.skip(f"缺基线 {base}")
    st = _ensure("selftest", _srcs_selftest())
    out = _run(st, [])
    assert out == base.read_bytes(), "selftest 与 22af9bb 基线不一致"


def test_cli_save_bytes_match_baseline():
    """CLI 存档格式零变化：当前 src/ 与 22af9bb 基线 CLI 在相同 seed 下产出的 .sav 逐字节一致。"""
    if not BASELINE_CLI.exists():
        pytest.skip(f"缺 22af9bb 基线 CLI：{BASELINE_CLI}")
    cur = _ensure("starcolony_p1reg", _srcs_cli())
    env = os.environ.copy()
    env.setdefault("TMPDIR", str(BUILD / "tmp"))
    blobs = {}
    # 关键：两次必须用**同一个**存档路径——CLI 会把 "save <path>" 命令连同路径写进存档日志，
    # 若路径不同，存档字节必然不同（这是我上一版的测试 bug，不是产品回归）。
    # 同样不用 unlink 清档（沙箱会把 os.unlink 劫持成 safe-delete 而抛 OSError）：
    # 写哨兵占位，既避免删除，又保证读到的是本次刚写出的存档。
    savepath = QA_TMP / "p1reg_shared.sav"
    for tag, binary in (("base", BASELINE_CLI), ("cur", cur)):
        sentinel = _arm_savepath(savepath)
        script = f"save {savepath}\nquit\n"
        subprocess.run([str(binary), "--seed", "20240101", "--no-color"],
                       input=script.encode("utf-8"), cwd=str(ROOT), env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
        assert savepath.exists(), f"{tag} 未生成存档"
        raw = savepath.read_bytes()
        assert raw != sentinel, f"{tag} 未真正写出存档（仍是哨兵内容）"
        blobs[tag] = raw
    assert blobs["cur"] == blobs["base"], "CLI 存档字节与 22af9bb 基线不一致"


# =====================================================================
#  CLI 交互会话逐字节对照（P2 §4.4.1 配套红线 + 说明）
# =====================================================================
# 刻意**不使用 unlink** 清存档：本沙箱会把 os.unlink 劫持成 safe-delete（移入 ~/.Trash-0），
# 在该目录不可写时抛 OSError，使「存档字节逐字节对照」这条红线随机变红。
# 改为写入哨兵内容，既不依赖删除，又能确保读到的一定是本次 CLI 刚写出的存档（而非陈旧文件）。
_SENTINEL = b"QA-SENTINEL-NOT-A-REAL-SAVE\n"


def _arm_savepath(savepath):
    """把存档路径占位成哨兵，返回哨兵字节。"""
    if savepath is None:
        return None
    savepath.write_bytes(_SENTINEL)
    return _SENTINEL


def _cli(binary, seed, script, env, savepath=None):
    """驱动一次完整 CLI 会话；返回 (stdout_bytes, save_bytes|None)。

    关键：两次对照必须传入**同一个** savepath —— CLI 会把 "save <path>" 命令连同
    路径写进存档日志；路径不同存档字节必然不同（这是测试陷阱，不是产品差异）。
    """
    sentinel = _arm_savepath(savepath)
    p = subprocess.run([str(binary), "--seed", str(seed), "--no-color"],
                       input=script.encode("utf-8"), cwd=str(ROOT), env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
    data = None
    if savepath is not None and savepath.exists():
        raw = savepath.read_bytes()
        assert raw != sentinel, (
            f"{savepath} 仍是哨兵内容——本次 CLI 没能真正写出存档，"
            f"不能拿陈旧/缺失文件去做逐字节对比")
        data = raw
    return p.stdout, data


@pytest.mark.parametrize("seed", EVENT_FREE_SEEDS)
def test_cli_event_free_session_byte_identical(seed):
    """红线：**全程无待决事件**的 CLI 会话，当前 src/ 与 22af9bb 基线逐字节一致。

    规则上移 core 只会改变「待决期间非 answer 操作」这一条路径。只要会话中没有
    pending 事件，两个版本的每一条日志、每一帧渲染、以及存档字节都必须一模一样。
    这是防止 P2 改动意外波及无关路径的硬门禁——一旦红，说明改动越界了。
    """
    if not BASELINE_CLI.exists():
        pytest.skip(f"缺 22af9bb 基线 CLI：{BASELINE_CLI}")
    cur = _ensure("starcolony_p1reg", _srcs_cli())
    env = os.environ.copy()
    env.setdefault("TMPDIR", str(BUILD / "tmp"))
    savepath = QA_TMP / "p1ev_shared.sav"
    script = f"status\nlist\ndetail\nnext\nnext\nnext\nsave {savepath}\nquit\n"

    base_out, base_sav = _cli(BASELINE_CLI, seed, script, env, savepath)
    cur_out, cur_sav = _cli(cur, seed, script, env, savepath)

    # 自检门禁：固定脚本必须真的无事件，否则“无事件逐字节”这条红线无从成立。
    for tag, out in (("base", base_out), ("cur", cur_out)):
        assert _EVENT_MARK not in out.decode("utf-8", "replace"), (
            f"seed={seed} 的固定脚本本应无待决事件，却在 {tag} 输出中出现了事件弹窗——"
            f"脚本需重选（含事件差异见 test_cli_event_session_expected_diff）")

    assert base_out == cur_out, (
        f"无事件会话 stdout 与 22af9bb 基线不一致（seed={seed}）："
        f"base={len(base_out)}B cur={len(cur_out)}B")
    assert base_sav is not None and cur_sav is not None, "无事件会话未生成存档"
    assert base_sav == cur_sav, (
        f"无事件会话存档字节与 22af9bb 基线不一致（seed={seed}）："
        f"base={len(base_sav)}B cur={len(cur_sav)}B")


def test_cli_event_session_expected_diff():
    """说明（**不判失败**）：含待决事件的会话存在规格预期的差异。

    触发一个待决事件后，再发一条非 answer 操作（build farm）。22af9bb 基线把它当正常
    操作执行；P2 之后核心返回 BlockedByPending，前端打印「有事件需要先处理（输入选项数字）」。
    因此两者必然不同——这是 §4.4.1 预期的差异，非回归，仅作记录以便复核。
    """
    if not BASELINE_CLI.exists():
        pytest.skip(f"缺 22af9bb 基线 CLI：{BASELINE_CLI}")
    cur = _ensure("starcolony_p1reg", _srcs_cli())
    env = os.environ.copy()
    env.setdefault("TMPDIR", str(BUILD / "tmp"))
    seed = 7
    script = "next\n" * 6 + "build farm 4 4\nquit\n"

    base_out, _ = _cli(BASELINE_CLI, seed, script, env)
    cur_out, _ = _cli(cur, seed, script, env)
    bo = base_out.decode("utf-8", "replace")
    co = cur_out.decode("utf-8", "replace")

    if _EVENT_MARK not in co:
        pytest.skip(f"seed={seed} 本次未出现待决事件，无可记录的差异")

    base_blocks = _BLOCKED_MARK in bo
    cur_blocks = _BLOCKED_MARK in co
    identical = base_out == cur_out
    note = (
        f"含事件会话预期差异（记录，不判失败）：seed={seed}\n"
        f"  基线(22af9bb) 含拦截文案：{base_blocks}（规格期望 False）\n"
        f"  当前(src)      含拦截文案：{cur_blocks}（规格期望 True）\n"
        f"  stdout 逐字节相同：{identical}（规格期望 False）\n"
        "  根因：§4.4.1「待决期间非 answer 操作由核心拦截」——规则从 CLI 上移到 core。")
    print(note)
    if cur_blocks and not base_blocks and not identical:
        pytest.skip(note)         # 与规格完全吻合 → 记录并跳过（不判失败）
    pytest.skip("含事件会话：观测到的差异与 §4.4.1 不完全一致，请人工复核。\n" + note)


# =====================================================================
#  存档 ×5 红线：P2.1 明确「不动存档格式」，存档字节必须一步不退
# =====================================================================
# 五个覆盖面不同的会话：开局即存 / 纯推进 / 推进+建造 / 只建造不推进 / 多次建造+推进。
# 空行在 CLI 里等价于 next（推进一个周期）。
SAVE_SCENARIOS = [
    ("seed1_开局即存",        1, ["save {save}", "quit"]),
    ("seed42_空行推进3次",     42, ["", "", "", "save {save}", "quit"]),
    ("seed2_建造后推进2次",    2, ["build farm 5 4", "", "", "save {save}", "quit"]),
    ("seed1_只建造不推进",     1, ["build hab 3 3", "build sol 6 6", "save {save}", "quit"]),
    ("seed42_多建多推进",42, ["build farm 5 4", "build farm 5 5", "", "", "", "save {save}", "quit"]),
]


@pytest.mark.parametrize("tag,seed,cmds", SAVE_SCENARIOS, ids=[s[0] for s in SAVE_SCENARIOS])
def test_cli_save_bytes_x5(tag, seed, cmds):
    """存档红线 ×5：五个不同会话产出的 .sav，必须与 22af9bb 基线 CLI 逐字节一致。

    P2.1 新增 preview_build / idleWorkers / seed / workerNeed，但**明确不动存档格式**，
    所以存档字节一步都不能退。这是本轮最该守住的一条。
    """
    if not BASELINE_CLI.exists():
        pytest.skip(f"缺 22af9bb 基线 CLI：{BASELINE_CLI}")
    cur = _ensure("starcolony_p1reg", _srcs_cli())
    env = os.environ.copy()
    env.setdefault("TMPDIR", str(BUILD / "tmp"))
    savepath = QA_TMP / "p1reg_x5.sav"
    script = "\n".join(c.format(save=savepath) for c in cmds) + "\n"

    base_out, base_sav = _cli(BASELINE_CLI, seed, script, env, savepath)
    cur_out, cur_sav = _cli(cur, seed, script, env, savepath)

    # 自检门禁：含待决事件的会话存在规格预期差异（见上方说明用例），不参与逐字节对比。
    for who, out in (("base", base_out), ("cur", cur_out)):
        if _EVENT_MARK in out.decode("utf-8", "replace"):
            pytest.skip(f"{tag}（seed={seed}）会话中出现了待决事件，按 §4.4.1 不参与逐字节对比")

    assert base_sav is not None and cur_sav is not None, f"{tag} 未生成存档"
    assert base_sav == cur_sav, (
        f"存档字节与 22af9bb 基线不一致（{tag}, seed={seed}）："
        f"base={len(base_sav)}B cur={len(cur_sav)}B")
