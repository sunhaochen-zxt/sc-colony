"""星际争霸：殖民地 —— 图形化终端前端（Python + Textual）。

本包实现 docs/PROTOCOL.md（v1，已冻结）定义的 JSON-RPC 前端：

    frontend/starcolony_tui/
        rpc.py       子进程管理 + 行协议编解码 + 超时/退出/异常处理
        models.py    协议 JSON 的类型化封装（防御式解析，容忍未知字段）
        i18n.py      ActCode -> 配色/图标映射；地形显示偏好；待决事件表
        stdin_watch.py 非交互 stdin 的 EOF 看门（管道读到 EOF 时按 q 退出）
        app.py       Textual App 主类（持有 RpcClient 与游戏状态）
        screens/      surface（地表主屏）/ help / event / build / dialogs
        widgets/      可复用组件（地图、资源栏、建筑列表、科技、日志）

入口：``PYTHONPATH=frontend .venv/bin/python -m starcolony_tui``
"""

from __future__ import annotations

__version__ = "1.0.0"

__all__ = ["__version__"]
