"""The UI side of a backend: one method per ``g6_ui_api`` entry (``include/gta6mod/gta6mod.h``, "UI layer").

``gta6mod.ui`` (the module mods use) calls only these methods, through ``runtime.current().backend.ui``; it
never touches a DLL. Implementations:

* ``MockUi`` (``runtime/mock_ui.py``): pure Python, records every call as a transcript line (the same lines as
  the C mock in ``runtime/src/mock_backend.c``) and plays back scripted clicks and values.
* ``CAbiUi`` (``runtime/cabi.py``): ctypes over the ``g6_ui_api`` table of a runtime DLL (``gta6mod_mock.dll``
  offline; in-game, whatever embeds Python and hands it the runtime's table).

Values follow the C ABI: indices are 0-based, colors are 4-tuples of floats in 0..1, flags are the
``WINDOW_*`` bits below. Widget methods are only valid while a frame callback runs.
"""
from __future__ import annotations

import abc
from dataclasses import dataclass
from typing import Callable, Sequence

# g6_ui_api ABI version this layer speaks (gta6mod.h G6_UI_ABI_MAJOR / MINOR)
UI_ABI_MAJOR = 1
UI_ABI_MINOR = 0
IMGUI_VERSION = "1.91.9b"  # G6_UI_IMGUI_VERSION (informational for Python: no ImGui objects cross this layer)

# begin() window flags (G6_UI_WINDOW_*)
WINDOW_NO_TITLE_BAR = 0x01
WINDOW_NO_RESIZE = 0x02
WINDOW_NO_MOVE = 0x04
WINDOW_NO_COLLAPSE = 0x08
WINDOW_AUTO_RESIZE = 0x10
WINDOW_NO_BACKGROUND = 0x20
WINDOW_NO_INPUTS = 0x40
FRAME_ALWAYS = 0x1  # G6_UI_FRAME_ALWAYS


@dataclass(frozen=True)
class Frame:
    """What a frame callback receives (g6_ui_frame)."""
    menu_open: bool   # the menu is shown and has the mouse and keyboard
    width: float      # back buffer size in pixels
    height: float
    dt: float         # seconds since the previous frame
    frame: int        # frame counter


FrameFn = Callable[[Frame], None]
Color = tuple[float, float, float, float]


class UiBackend(abc.ABC):
    """One method per g6_ui_api entry."""

    @abc.abstractmethod
    def frame_register(self, name: str, fn: FrameFn, flags: int) -> int | None:
        """Register ``fn``; return its id, or None when the host has no UI (overlay off: G6_E_NOT_IMPLEMENTED)."""

    @abc.abstractmethod
    def frame_unregister(self, frame_id: int) -> bool: ...

    # widgets (valid only inside a frame callback)
    @abc.abstractmethod
    def begin(self, title: str, closable: bool, flags: int) -> tuple[bool, bool]:
        """(contents visible, still open). end() must follow whatever it returned."""

    @abc.abstractmethod
    def end(self) -> None: ...

    @abc.abstractmethod
    def set_next_window_pos(self, x: float, y: float, always: bool) -> None: ...

    @abc.abstractmethod
    def set_next_window_size(self, w: float, h: float, always: bool) -> None: ...

    @abc.abstractmethod
    def text(self, text: str) -> None: ...

    @abc.abstractmethod
    def text_colored(self, rgba: Color, text: str) -> None: ...

    @abc.abstractmethod
    def button(self, label: str) -> bool: ...

    @abc.abstractmethod
    def checkbox(self, label: str, value: bool) -> tuple[bool, bool]:
        """(changed, value)"""

    @abc.abstractmethod
    def slider_int(self, label: str, value: int, lo: int, hi: int) -> tuple[bool, int]: ...

    @abc.abstractmethod
    def slider_float(self, label: str, value: float, lo: float, hi: float, fmt: str | None) -> tuple[bool, float]: ...

    @abc.abstractmethod
    def input_text(self, label: str, text: str, max_bytes: int) -> tuple[bool, str]: ...

    @abc.abstractmethod
    def combo(self, label: str, index: int, items: Sequence[str]) -> tuple[bool, int]: ...

    @abc.abstractmethod
    def separator(self) -> None: ...

    @abc.abstractmethod
    def same_line(self) -> None: ...

    @abc.abstractmethod
    def tree_node(self, label: str) -> bool: ...

    @abc.abstractmethod
    def tree_pop(self) -> None: ...

    @abc.abstractmethod
    def begin_tab_bar(self, tab_bar_id: str) -> bool: ...

    @abc.abstractmethod
    def end_tab_bar(self) -> None: ...

    @abc.abstractmethod
    def begin_tab_item(self, label: str) -> bool: ...

    @abc.abstractmethod
    def end_tab_item(self) -> None: ...

    @abc.abstractmethod
    def color_edit(self, label: str, rgba: Color, no_alpha: bool) -> tuple[bool, Color]: ...

    @abc.abstractmethod
    def plot_lines(self, label: str, values: Sequence[float], overlay: str | None, scale_min: float, scale_max: float,
                   width: float, height: float) -> None: ...
