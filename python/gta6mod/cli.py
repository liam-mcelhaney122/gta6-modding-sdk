"""gta6mod command line.

  python -m gta6mod info     <dump_dir>
  python -m gta6mod rpf9     <file.rpf>...
  python -m gta6mod rds5     <file.ttpf>...
  python -m gta6mod joaat    <string>...
  python -m gta6mod anchors  verify   --exe <game_scarlett_master.exe>
  python -m gta6mod anchors  sign     --exe <exe> [--write] [--force]
  python -m gta6mod anchors  manifest --exe <exe> -o manifest.json
  python -m gta6mod patterns check    --exe <exe>        (CI gate: every pattern matches once)
  python -m gta6mod patterns list     [--confidence high]
  python -m gta6mod patterns resolve  --exe <exe> --name <anchor> [--name ...]
  python -m gta6mod natives  [--status unverified|verified]
  python -m gta6mod run      <mod.py> [--ms 5000] [--backend mock|cabi:<dll>] [--press INPUT_CONTEXT@1000[+HOLD]]
                             [--damage N@MS] [--wanted N@MS] [--waypoint X,Y,Z@MS]   (scenario.py)
"""
from __future__ import annotations

import argparse
import json
import os
import runpy
import sys

from . import GAME_BUILD, __version__
from .joaat import joaat

KNOWN_EXE_SHA256 = {
    "e95020b2fa4a721db3df59fdecf75621730ac559d8835252f6c3b658d5a08993": "1.0.2.1 (Xbox Series, GDK)",
}


def _print(obj) -> None:
    print(json.dumps(obj, indent=2, default=lambda o: hex(o) if isinstance(o, int) else str(o)))


def cmd_info(a) -> int:
    from .formats.rpf9 import read_rpf9_header
    from .pe import PEImage

    exe = os.path.join(a.dump, "game_scarlett_master.exe")
    img = PEImage(exe)
    print(f"exe       : {exe}")
    print(f"sha256    : {img.sha256}")
    print(f"build     : {KNOWN_EXE_SHA256.get(img.sha256, 'UNKNOWN BUILD - anchors/natives may not apply')}")
    print(f"functions : {len(img.functions())} (.pdata)")
    print(f"sections  : {', '.join(s.name for s in img.sections)}")
    print("archives  :")
    for root, _dirs, files in os.walk(a.dump):
        for f in sorted(files):
            if f.lower().endswith(".rpf"):
                p = os.path.join(root, f)
                try:
                    h = read_rpf9_header(p)
                    print(f"  {os.path.relpath(p, a.dump):55} entries={h.entry_count:<6} key={h.key_id:#010x} "
                          f"toc_transform={'on' if h.toc_transform_enabled else 'off'}(codec {h.codec_descriptor_id}) "
                          f"sentinel={'ok' if h.sentinel_ok else 'DIFF'}")
                except ValueError as e:
                    print(f"  {f}: {e}")
    return 0


def cmd_rpf9(a) -> int:
    from .formats.rpf9 import read_rpf9_header
    for p in a.files:
        _print({"file": p, **read_rpf9_header(p).to_dict()})
    return 0


def cmd_rds5(a) -> int:
    from .formats.rds5 import read_rds5_header
    for p in a.files:
        h = read_rds5_header(p, max_records=8)
        _print({"file": p, "header": h.to_dict() if h else "raw (no RDS5 header)"})
    return 0


def cmd_joaat(a) -> int:
    for s in a.strings:
        h = joaat(s)
        print(f"{s:30} 0x{h:08X}  {h - (1 << 32) if h & 0x80000000 else h}")
    return 0


def cmd_anchors(a) -> int:
    from . import anchors as A
    from .pe import PEImage

    img = PEImage(a.exe)
    if img.sha256 not in KNOWN_EXE_SHA256:
        print(f"warning: exe {img.sha256[:16]}… is not a known build; verifying signatures anyway", file=sys.stderr)
    items, src = A.load_anchors(GAME_BUILD, img.image_base, a.source)
    print(f"{len(items)} anchors from {src}", file=sys.stderr)
    if a.action == "sign":
        report = A.sign_all(img, items, force=a.force)
        for k, v in report.items():
            print(f"  {k:40} {v}")
        if a.write:
            if src.endswith((".yaml", ".yml")):
                print("refusing to rewrite a YAML re-db file; pass --source to a JSON seed", file=sys.stderr)
                return 1
            meta = json.load(open(src, encoding="utf-8")).get("_meta", {})
            A.save_seed(items, src, meta, img.image_base)
            print(f"wrote signatures to {src}")
        return 0
    if a.action == "verify":
        rows = A.verify_all(img, items)
        bad = 0
        for r in rows:
            exp = f"{r['expected_rva']:#x}" if r["expected_rva"] is not None else "-"
            got = f"{r['resolved_rva']:#x}" if r["resolved_rva"] is not None else "-"
            print(f"  {r['status']:12} {r['name']:40} expected {exp:>11} resolved {got:>11} [{r['confidence']}]")
            bad += r["status"] not in ("ok",)
        print(f"{len(rows) - bad}/{len(rows)} anchors resolve uniquely to their recorded address")
        return 1 if bad else 0
    if a.action == "manifest":
        man = A.build_manifest(img, items, GAME_BUILD)
        with open(a.output, "w", encoding="utf-8") as fh:
            json.dump(man, fh, indent=2)
        print(f"wrote {a.output} ({len(man['anchors'])} anchors)")
        return 0
    return 2


def cmd_patterns(a) -> int:
    from . import patterns as P
    from .pe import PEImage

    if a.action == "check":
        if not a.exe:
            print("patterns check needs --exe", file=sys.stderr)
            return 2
        return P.check(a.exe)
    if a.action == "list":
        doc, _ = P.load_index()
        for e in doc["patterns"]:
            if a.confidence and e.get("confidence") != a.confidence:
                continue
            print(f"  {e['name']:44} {e['kind']:8} {e.get('expected_rva') if e.get('expected_rva') is None else hex(e['expected_rva']):>12} "
                  f"{'unsigned' if e.get('unsigned') else e['signature'][:48]}")
        return 0
    if a.action == "resolve":
        if not a.exe or not a.name:
            print("patterns resolve needs --exe and --name", file=sys.stderr)
            return 2
        doc, _ = P.load_index()
        by = {e["name"]: e for e in doc["patterns"]}
        img = PEImage(a.exe)
        bad = 0
        for name in a.name:
            e = by.get(name)
            if e is None:
                print(f"  {name}: no such anchor")
                bad += 1
                continue
            rva, status = P.resolve(img, e)
            print(f"  {status:10} {name:44} -> {'-' if rva is None else hex(rva)}")
            bad += status != "ok"
        return 1 if bad else 0
    return 2


def cmd_natives(a) -> int:
    from .natives import BY_HASH
    for h, (ns, name, sig, status) in sorted(BY_HASH.items(), key=lambda kv: (kv[1][0], kv[1][1])):
        if a.status and status != a.status:
            continue
        print(f"0x{h:016X}  {ns:12} {name:34} {sig:14} {status}")
    return 0


def cmd_run(a) -> int:
    from . import runtime, script

    if a.backend == "mock":
        rt = runtime.use_mock()
    elif a.backend.startswith("cabi:"):
        rt = runtime.use_cabi(a.backend[5:], allow_unverified=a.allow_unverified)
    else:
        print(f"unknown backend {a.backend}", file=sys.stderr)
        return 2
    from . import scenario

    events = [scenario.parse(kind, spec) for kind in scenario.KINDS for spec in getattr(a, kind) or []]
    backend = rt.backend
    needs = {"press": "press_control", "damage": "damage_player", "wanted": "set_wanted", "waypoint": "set_waypoint"}
    unsupported = sorted({e.kind for e in events if not hasattr(backend, needs[e.kind])})
    if unsupported:
        print(f"note: backend {a.backend} ignores scenario events: {', '.join(unsupported)}", file=sys.stderr)
        events = [e for e in events if e.kind not in unsupported]
    runpy.run_path(a.mod, run_name="__gta6mod__")
    sched = script.scheduler()
    t = scenario.drive(sched, backend, a.ms, events)
    alive = [s.name for s in sched.scripts if not s.done]
    errors = [s.name for s in sched.scripts if s.error]
    print(f"ran {t} ms; {rt.call_count} native calls; live scripts: {alive}; errored: {errors}")
    stop_errors = sched.stop()  # unload: the stop event switches toggles off and cancels the scripts
    print(f"stop event: {len(stop_errors)} cleanup error(s)")
    return 1 if errors or stop_errors else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="gta6mod", description=f"GTA VI modding SDK v{__version__} (game build {GAME_BUILD})")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("info"); p.add_argument("dump"); p.set_defaults(fn=cmd_info)
    p = sub.add_parser("rpf9"); p.add_argument("files", nargs="+"); p.set_defaults(fn=cmd_rpf9)
    p = sub.add_parser("rds5"); p.add_argument("files", nargs="+"); p.set_defaults(fn=cmd_rds5)
    p = sub.add_parser("joaat"); p.add_argument("strings", nargs="+"); p.set_defaults(fn=cmd_joaat)
    p = sub.add_parser("anchors"); p.add_argument("action", choices=["verify", "sign", "manifest"])
    p.add_argument("--exe", required=True); p.add_argument("--source"); p.add_argument("--write", action="store_true")
    p.add_argument("--force", action="store_true"); p.add_argument("-o", "--output", default="address_manifest.json")
    p.set_defaults(fn=cmd_anchors)
    p = sub.add_parser("patterns"); p.add_argument("action", choices=["check", "list", "resolve"])
    p.add_argument("--exe"); p.add_argument("--name", action="append"); p.add_argument("--confidence")
    p.set_defaults(fn=cmd_patterns)
    p = sub.add_parser("natives"); p.add_argument("--status"); p.set_defaults(fn=cmd_natives)
    p = sub.add_parser("run"); p.add_argument("mod"); p.add_argument("--ms", type=int, default=5000)
    p.add_argument("--backend", default="mock"); p.add_argument("--allow-unverified", action="store_true")
    p.add_argument("--press", action="append", help="CONTROL@MS[+HOLD], e.g. INPUT_CONTEXT@1008 or INPUT_CONTEXT@1008+600 (mock only)")
    p.add_argument("--damage", action="append", help="N@MS: hurt the player ped and its vehicle (mock only)")
    p.add_argument("--wanted", action="append", help="N@MS: set the wanted level (mock only)")
    p.add_argument("--waypoint", action="append", help="X,Y,Z@MS: set the map waypoint (mock only)")
    p.set_defaults(fn=cmd_run)
    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
