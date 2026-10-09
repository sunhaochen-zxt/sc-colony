"""屏幕集合：地表主屏、帮助、待决事件、建造菜单、通用对话框。"""

from __future__ import annotations

from .surface import SurfaceScreen
from .help import HelpScreen, HelpPanel
from .event import EventScreen
from .build import BuildMenu
from .dialogs import MessageScreen, PromptScreen

__all__ = [
    "SurfaceScreen",
    "HelpScreen",
    "HelpPanel",
    "EventScreen",
    "BuildMenu",
    "MessageScreen",
    "PromptScreen",
]
