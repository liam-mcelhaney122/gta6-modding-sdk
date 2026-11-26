"""Test mod (g6_python_test): the embedded interpreter's environment and the _g6 contract, checked from inside."""
import ctypes
import sys
import threading

import _g6
from gta6mod import runtime, script
from gta6mod.runtime.embedded import EmbeddedBackend

problems = []


def need(cond, what):
    if not cond:
        problems.append(what)


need(sys.flags.isolated == 1, f"isolated={sys.flags.isolated}")
need(sys.flags.no_site == 1, f"no_site={sys.flags.no_site}")
need(not any("site-packages" in p.lower() for p in sys.path), f"site-packages on sys.path: {sys.path}")
need("_g6" in sys.builtin_module_names, "_g6 is not a built-in module")
need(__name__ == "g6mods.env_check", f"__name__={__name__}")
need(isinstance(runtime.current().backend, EmbeddedBackend), "backend is not EmbeddedBackend")
need("game_scarlett" not in sys.executable.lower(), f"sys.executable={sys.executable}")


@script.register("checks")
def checks():
    b = runtime.current().backend
    need(_g6.on_script_thread(), "on_script_thread() False inside a tick")
    probe = b.ui_proc("g6_ui_harness_probe")
    need(probe != 0, "ui_proc(g6_ui_harness_probe) = 0")
    if probe:
        need(ctypes.CFUNCTYPE(ctypes.c_int)(probe)() == 42, "g6_ui_harness_probe() != 42")
    need(b.ui_proc("g6_get_host_api") == 0, "ui_proc resolved a non-g6_ui_ export")
    need(_g6.ui_proc("g6_get_host_api") == 0, "_g6.ui_proc resolved a non-g6_ui_ export")
    status, _ = _g6.invoke(0x4F67E8ECA7D3F667, [])  # GET_GAME_TIMER on the script thread
    need(status == 0, f"GET_GAME_TIMER status {status}")
    seen = {}

    def other_thread():
        seen["status"] = _g6.invoke(0x4F67E8ECA7D3F667, [])[0]
        seen["on"] = _g6.on_script_thread()

    t = threading.Thread(target=other_thread)
    t.start()
    t.join()
    need(seen.get("status") == 5, f"native from a mod thread: status {seen.get('status')} (want 5)")
    need(seen.get("on") is False, "on_script_thread() True on a mod thread")
    try:
        _g6.invoke(1, [0] * 33)
        need(False, "33 slots accepted")
    except ValueError:
        pass
    print("hello from print")
    runtime.current().log("INFO", "env_check", "env ok" if not problems else "env FAIL: " + "; ".join(problems))
    yield script.wait(100000)
