"""``python -m starcolony_tui`` 入口。

用法（从仓库根）：

    PYTHONPATH=frontend .venv/bin/python -m starcolony_tui
    # 或
    (cd frontend && ../.venv/bin/python -m starcolony_tui)

也支持直接执行本文件：

    .venv/bin/python frontend/starcolony_tui/__main__.py

自举：当以「脚本」方式运行时（``__package__`` 为空），把 ``frontend/`` 放进 ``sys.path``，
使包内相对导入正常工作。
"""

from __future__ import annotations

import sys
from pathlib import Path

if __package__ in (None, ""):  # 直接执行文件：把 frontend/ 加入 sys.path
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from starcolony_tui.app import main  # type: ignore[import-not-found]
else:
    from .app import main


if __name__ == "__main__":
    sys.exit(main())
