"""Run-length UI transcript logs (sdk/tests/ui/*.ui.txt), the format of g6mock::UiLog in runtime/src/mock_world.cpp.

"@ FIRST[-LAST] ms" starts a run of consecutive frames (one per 16 ms tick) with the same transcript; a tick that
drew no frame ends a run. Leading '#' lines of a file say how it was made.
"""
import os

SDK = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
UI_LOGS = os.path.join(SDK, "tests", "ui")
FRAME_MS = 16


class UiLog:
    def __init__(self):
        self.runs = []      # [first, last, transcript]
        self._closed = False

    def add(self, ms, transcript):
        if self.runs and not self._closed and self.runs[-1][2] == transcript:
            self.runs[-1][1] = ms
        else:
            self.runs.append([ms, ms, transcript])
        self._closed = False

    def gap(self):
        self._closed = True

    def text(self):
        return "".join(f"@ {a}{'' if a == b else f'-{b}'} ms\n{t}" for a, b, t in self.runs)


def read(name_or_path):
    path = name_or_path if os.path.isabs(name_or_path) else os.path.join(UI_LOGS, name_or_path)
    with open(path, encoding="utf-8", newline="") as fh:
        lines = fh.read().replace("\r\n", "\n").split("\n")
    while lines and lines[0].startswith("#"):
        lines.pop(0)
    return "\n".join(lines)


def frames(text):
    """{ms: transcript} for every frame of a log text."""
    out, ms, cur = {}, None, []
    runs = []
    for line in text.split("\n"):
        if line.startswith("@ "):
            if ms is not None:
                runs.append((ms, "".join(x + "\n" for x in cur)))
            ms, cur = line[2:-3], []
        elif line or cur:
            cur.append(line)
    if ms is not None:
        while cur and cur[-1] == "":
            cur.pop()
        runs.append((ms, "".join(x + "\n" for x in cur)))
    for span, t in runs:
        a, _, b = span.partition("-")
        for m in range(int(a), int(b or a) + 1, FRAME_MS):
            out[m] = t
    return out
