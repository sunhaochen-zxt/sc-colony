#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# 星际争霸：殖民地 (Star Colony) —— P3a「启动即拒绝」路径测试（QA / Edward）
#
# 为什么单独成文件：
#   content_validator_adversarial.py 用探针直接调 loadContentPack/initContent，测的是
#   **校验函数本身**（该不该拒、信息可不可定位）。本文件测的是 **引擎启动路径**——
#   契约 §2 原话：「任何一个文件缺失或校验失败都必须让引擎启动失败……不得静默使用默认值」。
#   两者是不同层次：函数拒了，不代表 `main()`/`ensureContent()` 真的把启动拦住。
#
# 本文件钉住一条此前真实存在、现已修复的契约缺口的**回归**（见 test_explicit_content_dir_not_overridden）：
#   显式 SC_CONTENT_DIR 指向「存在但非法」的内容包时，必须拒启动；不得因 exe 同级恰好有合法
#   content/base 而静默回退（§2「不得静默使用默认值」）。修复点：src/content.cpp ensureContent()
#   仅在目录**未被显式指定**时才尝试同级候选（!dirExplicitRef()）。
#
# 运行：
#   export TMPDIR=/home/shc/starcolony/build/tmp
#   .venv/bin/python -m pytest frontend/tests/content_startup_refusal_check.py -v -rs
from __future__ import annotations

import json
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
CONTENT = ROOT / "content" / "base"
CLI_BIN = ROOT / "build" / "starcolony"
WORK = ROOT / "build" / "tmp" / "qa_startup"

FILES = ("manifest.json", "buildings.json", "techs.json", "weathers.json", "tuning.json")


def _has_content() -> bool:
    return all((CONTENT / n).exists() for n in FILES)


requires_content = pytest.mark.skipif(
    not _has_content(), reason="content/base 尚未落地 —— 启动路径测试需真实内容包")
requires_cli = pytest.mark.skipif(
    not CLI_BIN.exists(), reason=f"缺少 {CLI_BIN}（先 make 出 CLI）")


def _fresh(dirname: str) -> Path:
    d = WORK / dirname
    if d.exists():
        shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True, exist_ok=True)
    return d


def _bad_pack(name: str) -> Path:
    """复制 content/base 到临时目录并把 startMetal 删掉，制造一处**必被拒**的校验失败。"""
    d = _fresh(f"pack_{name}")
    for fn in FILES:
        obj = json.loads((CONTENT / fn).read_text(encoding="utf-8"))
        (d / fn).write_text(json.dumps(obj, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    tune = json.loads((d / "tuning.json").read_text(encoding="utf-8"))
    tune.pop("startMetal")          # §8：缺必填字段 → 必拒
    (d / "tuning.json").write_text(json.dumps(tune, ensure_ascii=False, indent=2) + "\n",
                                   encoding="utf-8")
    return d


def _run_cli(cli: Path, content_dir: Path | None) -> subprocess.CompletedProcess:
    env = {"PATH": "/usr/bin:/bin",
           "TMPDIR": str(ROOT / "build" / "tmp"),
           "HOME": str(ROOT / "build" / "tmp")}
    if content_dir is not None:
        env["SC_CONTENT_DIR"] = str(content_dir)
    return subprocess.run([str(cli), "--seed", "1", "--no-color"], cwd=str(ROOT), env=env,
                          stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=60)


# ---------------------------------------------------------------------------
#  1) 合法内容包：正常启动（防「一律拒绝」的假阳性）
# ---------------------------------------------------------------------------
@requires_content
@requires_cli
def test_valid_pack_starts_ok():
    iso = _fresh("ok")
    cli = iso / "starcolony"
    shutil.copy2(CLI_BIN, cli)
    p = _run_cli(cli, CONTENT)
    assert p.returncode == 0, f"合法内容包竟无法启动：\n{p.stderr}"
    assert "内容加载失败" not in p.stderr, p.stderr


# ---------------------------------------------------------------------------
#  2) 坏内容包 + 无候选可回退：必须拒启动并报可定位错误（§2 / §8）
# ---------------------------------------------------------------------------
@requires_content
@requires_cli
def test_bad_pack_refused_when_no_fallback_available():
    iso = _fresh("isolated")
    cli = iso / "starcolony"
    shutil.copy2(CLI_BIN, cli)
    bad = _bad_pack("isolated")
    p = _run_cli(cli, bad)
    assert p.returncode != 0, f"坏内容竟启动成功（exit={p.returncode}）\n{p.stdout}"
    assert "内容加载失败" in p.stderr, p.stderr
    assert "tuning.json" in p.stderr and "startMetal" in p.stderr, p.stderr
    assert "缺少必填字段" in p.stderr, p.stderr


# ---------------------------------------------------------------------------
#  3) 显式 SC_CONTENT_DIR 指向非法包时不得被静默覆盖（契约 §2）—— 当前为已知违例
# ---------------------------------------------------------------------------
@requires_content
@requires_cli
def test_explicit_content_dir_not_overridden():
    """exe 同级放一份**合法** content/base（模拟从 build/ 运行），再用 SC_CONTENT_DIR 指向非法包。

    契约（§2）：显式指定的包若非法 → 必须拒启动，不得静默改用同级那份合法包。
    回归：曾出现静默回退（exit 0、零报错），已在 ensureContent() 加 dirExplicitRef() 判定后修复。
    """
    mask = _fresh("mask")
    cli = mask / "starcolony"
    shutil.copy2(CLI_BIN, cli)
    shutil.copytree(CONTENT, mask / "content" / "base")   # 同级合法包 —— 修复前会成为静默回退目标
    bad = _bad_pack("override")
    p = _run_cli(cli, bad)
    assert p.returncode != 0, (
        "显式 SC_CONTENT_DIR 指向非法内容包，却被静默回退到 exe 同级候选目录并以 exit 0 启动。\n"
        f"exit={p.returncode}\nstderr={p.stderr!r}")
    assert "内容加载失败" in p.stderr and "startMetal" in p.stderr, p.stderr


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v", "-rs"]))
