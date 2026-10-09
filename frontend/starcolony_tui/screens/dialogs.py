"""通用对话框：信息提示（可带按钮）与单行文本输入。"""

from __future__ import annotations

from typing import Optional, Union

from rich.text import Text
from textual.binding import Binding
from textual.containers import Horizontal, Vertical, VerticalScroll
from textual.screen import ModalScreen
from textual.widgets import Button, Input, Static


class MessageScreen(ModalScreen[Optional[str]]):
    """标题 + 正文（可滚动）+ 可选按钮；``dismiss`` 返回被按下的按钮标签或 ``None``。"""

    BINDINGS = [Binding("escape", "cancel", "关闭")]

    DEFAULT_CSS = """
    MessageScreen { align: center middle; }
    MessageScreen > Vertical {
        width: 74; height: auto; max-height: 80%;
        border: round $accent; background: $surface; padding: 1 2;
    }
    MessageScreen #dlg-title { text-style: bold; color: $accent; height: auto; }
    MessageScreen VerticalScroll { height: auto; max-height: 20; }
    MessageScreen Horizontal { height: auto; align-horizontal: right; }
    MessageScreen Button { margin-left: 2; }
    """

    def __init__(self, title: str, body: Union[str, Text] = "", buttons: Optional[list[str]] = None) -> None:
        super().__init__()
        self._title = title
        self._body = body
        self._buttons = list(buttons or [])
        # 按钮 id 只能用 [A-Za-z0-9_-]，故用序号，再映射回中文标签
        self._button_labels: dict[str, str] = {}

    def compose(self):
        with Vertical():
            yield Static(self._title, id="dlg-title")
            with VerticalScroll():
                yield Static(self._body if isinstance(self._body, Text) else Text(str(self._body)),
                             id="dlg-body", markup=False)
            if self._buttons:
                with Horizontal():
                    for i, label in enumerate(self._buttons):
                        button_id = f"btn-{i}"
                        self._button_labels[button_id] = label
                        yield Button(label, id=button_id)

    def on_button_pressed(self, event: Button.Pressed) -> None:
        event.stop()
        self.dismiss(self._button_labels.get(event.button.id or "", None))

    def action_cancel(self) -> None:
        self.dismiss(None)


class PromptScreen(ModalScreen[Optional[str]]):
    """单行文本输入；``dismiss`` 返回输入值或 ``None``（取消）。"""

    BINDINGS = [Binding("escape", "cancel", "取消")]

    DEFAULT_CSS = """
    PromptScreen { align: center middle; }
    PromptScreen > Vertical {
        width: 64; height: auto; border: round $accent; background: $surface; padding: 1 2;
    }
    PromptScreen #prompt-title { text-style: bold; color: $accent; height: auto; }
    PromptScreen #prompt-hint { color: $text-muted; height: auto; }
    """

    def __init__(self, title: str, value: str = "", hint: str = "Enter 确认 · Esc 取消") -> None:
        super().__init__()
        self._title = title
        self._value = value
        self._hint = hint

    def compose(self):
        with Vertical():
            yield Static(self._title, id="prompt-title")
            yield Input(value=self._value, id="prompt-input")
            yield Static(self._hint, id="prompt-hint")

    def on_mount(self) -> None:
        self.query_one("#prompt-input", Input).focus()

    def on_input_submitted(self, event: Input.Submitted) -> None:
        event.stop()
        text = event.value.strip()
        self.dismiss(text if text else None)

    def action_cancel(self) -> None:
        self.dismiss(None)
