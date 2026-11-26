# gta6mod_runtime (in-process runtime, GTA VI 1.0.2.1)

`gta6mod_runtime.dll` implements the `gta6mod.h` host ABI inside the process that runs
`game_scarlett_master.exe` under ztrx/VIctory. It follows `docs/runtime_design.md`, with the
deviations listed under "Design changes". **Nothing here has run in-game.** The verified
results are offline only: the build, the self-test, and readonly runs against the exe *file*
mapped into memory.

Single-player only. NETWORK natives are always refused. Every other native, and the mods/
overlay, is refused while the single-player guard is closed (see "Single-player guard").

## Files

```
gen_manifest_table.py        manifest JSON (+ original exe) -> src/manifest_1_0_2_1.inc
src/manifest_1_0_2_1.inc     GENERATED: identity pins, anchor RVAs, 6,829 native hash->handler RVAs
src/build_consts.hpp         every other build-dependent offset, each with its provenance
src/runtime.cpp              init sequence, Run detour, TLS publish, guard, host API, plugins, exports
src/identity.*               module discovery, header/export fingerprint, imphash, file SHA-256 (BCrypt)
src/pattern_scan.*           one-pass multi-pattern scanner (the same semantics as patterns.hpp find+deref)
src/x64_decode.*             x64 length decoder (refuses anything it does not fully decode)
src/hook.*                   relocating trampoline builder + inline hook (suspend, check RIPs, patch, roll back)
src/natives.*                static handler self-id check, SEH-guarded invoker
src/scheduler.*              fiber-per-script scheduler, script_wait
src/guard.*                  single-player guard: address classifier, gate, GetExtendedTcpTable worker
src/game_clock.*             game_time_ms: clock block from the GET_GAME_TIMER handler, validator/mixer
src/vfs.*                    mods/ overlay: ABI checks, mount-table dump, Relative device + MountAs on the script thread
tools/disasm_fn.py           read-only capstone listing of exe VAs (the evidence for the two items above)
src/log.*                    gta6mod_runtime.log next to the DLL (previous run kept as .log.prev)
src/g6rt_test_api.h          offline test surface (g6rt_get_test_api export)
tests/loader_selftest.cpp    offline test exe
gta6mod_runtime.ini          default config (copied to build\ on the first build)
build.cmd / build.ps1        clang-cl build, x64, C++20, /W4 /WX, static CRT (also calls build_inject.cmd)
tools/inject/g6inject.cpp    g6inject.exe: waits for the DexNom title process, LoadLibraryW's one DLL into it
tools/inject/inject_core.hpp g6inject argument parsing, name matching, exit codes (pure, tested offline)
build_inject.cmd             builds g6inject.exe and the injector test binaries into build\ and build\inject_test\
tests/inject/*.cpp           injector test: stand-in target, image-named dummy DLL, marker DLL, driver
plugins/g6probe/             g6probe.dll: live probe plugin (mode=full) writing JSONL; see tools/live/stages/README.md
build_probe.cmd              builds build\probe\g6probe.dll and its tests into build\probe_test
tests/probe/*.cpp            g6probe unit test, mock-world test (gta6mod_mock.dll + fake title image)
ui/g6ui.cpp, ui/g6ui.h       g6ui.dll: Dear ImGui overlay on the host's D3D12 swap chain ("UI overlay" below)
ui/ui_logic.hpp              its pure decisions: queue match, window-message handling, mouse scale (tested offline)
plugins/g6trainer/           g6trainer.dll: C++ example plugin with a menu (gta6mod/ui.hpp)
build_ui.cmd                 builds build\g6ui.dll, build\trainer\g6trainer.dll and the UI tests into build\ui_test
tests/ui/ui_test.cpp         overlay test on its own D3D12 device + swap chain (hidden window); ui_test_plugin.cpp
                             draws with its own ImGui copy on the shared context; trainer_mock_test.cpp
../../third_party/imgui/     Dear ImGui v1.91.9b, vendored unchanged (G6_VENDOR.txt: source, commit, sha256)
../../python/host/           g6_python.dll: the in-game Python host plugin (see "Python plugin" below); its build.cmd
                             is called by build.cmd and writes build\python\ and build\python_test\
../src/ui_retained.{h,cpp}   retained UI for hosts that must not run on the render thread (g6_ui_retained_* exports;
                             docs/python_host.md "Python UI"); compiled into gta6mod_runtime.dll
```

## Build

```bat
sdk\runtime\loader\build.cmd test
```
`build.cmd` builds the runtime, the self-test, `g6inject.exe`, the `g6probe` plugin, the UI overlay (`g6ui.dll`,
`g6trainer.dll`) and the `g6_python` plugin; `test` runs the self-test, the injector test, the two g6probe tests, the
UI tests (`ui_test.exe` on the default adapter and on WARP, `trainer_mock_test.exe`) and `g6_python_test.exe`, and
the script exits non-zero if any fails.
`g6_python` needs CPython 3.12 headers and libs (`G6_PYTHON_HOME`, else `%LOCALAPPDATA%\Programs\Python\Python312`);
without them the plugin and its test print `SKIPPED` and the rest still builds and runs. (Before 2026-11-25 `build.cmd test`
returned the errorlevel from before the self-test ran, so a failing self-test still exited 0.)
```powershell
.\sdk\runtime\loader\build.ps1 -Test -Dump "<DUMP>"
.\sdk\runtime\loader\build.ps1 -Test -Low    # the whole build and test tree at idle priority
```
`build.ps1` exits 0 only if every build step and test passed, else the failing step's code (it names the step).
For a low-priority run use `-Low`, not `cmd /c "start /low /wait /b powershell -File build.ps1 ..."`: a `cmd /c`
whose only command is `start` exits 0 whatever the started process returned. Before 2026-11-25 `build.ps1` also
ran with `$ErrorActionPreference = "Stop"`, so a caller that redirected stderr aborted it on the first compiler
diagnostic instead of on an exit code.

The build uses `C:\Program Files\LLVM\bin\clang-cl.exe` (or `clang-cl` on PATH), linked with
lld-link. clang-cl finds the MSVC 14.52 BuildTools and Windows SDK 10.0.28000 on its own, so
no vcvars is needed. The flags are `/std:c++20 /O2 /W4 /WX /EHsc /MT`. No warning is suppressed.
The output in `build\` has these imports only: `KERNEL32.dll`, `bcrypt.dll` and `IPHLPAPI.DLL`.
`build\g6ui.dll` (the optional overlay) also imports `d3d12`, `dxgi`, `d3dcompiler_47`, `user32`, `gdi32`, `dwmapi`;
it is loaded only with `[ui] overlay=1` and `mode=full`.

Self-test (late-load round, offline): **344 passed** without `G6_DUMP`, **355 passed** with it
(round 3 was 308 / 319; the 36 new checks are the late-load patch-safety tests below). The
non-dump part includes the guard tests: v4/v6 address classes, the trip decision, the gate latch,
and a live `GetExtendedTcpTable` pass on the test process (must succeed with 0 own rows). The
dump run checks identity, all 469 anchors, 6,829 handlers, the three clock handlers decoded from the
file bytes (block RVA `0xabbef40`), and the vfs ABI checks (sizeof(Relative) 0x118, ctor RVA
`0x165fa50`).

CMake: `-DG6_WITH_RUNTIME_LOADER=ON` adds `gta6mod_runtime` and `g6_loader_selftest`
(`sdk/CMakeLists.txt`). There is no cmake on this machine, so that target has not been
configured. The verified path is `build.cmd`. The target lists `game_clock.cpp`, `vfs.cpp` and
`guard.cpp` and links `bcrypt` and `iphlpapi`, the same as `build.cmd` (not configured here).

After the manifest changes, regenerate the table:
`python -I sdk/runtime/loader/gen_manifest_table.py --manifest sdk/data/generated/address_manifest.1.0.2.1.json --exe <dump>\game_scarlett_master.exe`
The script refuses an exe whose SHA-256 is not the manifest's. It also checks its imphash
re-implementation against pefile and against the manifest.

## Architecture (init thread, started from DllMain; nothing heavy runs under the loader lock)

1. **Find the title image.** The runtime looks for a module that exports
   `__XMemAllocationHooks__`: loader list first, then allocation bases in memory.
   `GetModuleHandle(nullptr)` is the host, so it is never used. The runtime waits up to
   `wait_for_game_ms` for the image to appear.
2. **Identity** (see the next section). An unknown build gets `G6_E_UNSUPPORTED_BUILD`, and
   nothing else runs.
3. **Anchors.**
   - Every signed anchor in `patterns.hpp` is resolved over the executable sections with one
     matching pass. The pass uses a 2-byte-key index with the rarest-pair key, and takes
     about 160 ms for 95 MB.
   - Anchors with an empty signature but a `chains[]` entry are resolved through the
     generated `g6::patterns::resolve_anchor` (7 on 1.0.2.1).
   - Every result is compared with the manifest RVA. A mismatch is a warning, and the pattern
     or chain wins.
   - Required anchors (exactly one match, otherwise refuse):
     - `script.exec.run`;
     - `script.exec.vtable`, whose slot 17 must equal Run;
     - `script.native.handler.set_entity_coords`, which must equal the manifest handler RVA
       and pass the self-id check;
     - `script.native.lock_fn`.
   - Those four are re-resolved through `resolve_anchor` and must agree with the scanner.
     The self-test checks every 25th signed anchor the same way.
   - `strict_patterns=1` refuses on any non-exact anchor. The default is off, so an anchor the
     runtime does not need cannot stop it on a host that differs from the simulated one.
4. **Natives (static, read-only).** For each of the 6,829 manifest handlers, the runtime:
   - walks the prologue with the length decoder to `cmp dword [rcx+0x10],0` and the `js` after it;
   - follows the `js` and reads the `movabs` id;
   - requires `id == rotl64(~hash, 32)`.

   This read-only check replaces the call-based probe. Failures are logged and the native
   stays unusable (`G6_E_UNVERIFIED_NATIVE`).
5. **TLS publish site.** The bytes at Run+0x125 are classified as one of:
   - the console-faithful `mov rax, gs:[0x58]`;
   - a host `call <thunk>; nop4`;
   - unknown, which disables ticks.

   The tail `mov r14,[rax]; mov r12,[r14+0x1478]; mov [r14+0x1478],rsi` must match exactly.
6. **mode=readonly stops here.**
7. **Hook** (`mode=observe|full`): an inline detour on `scrExecContext::Run`.
   - Near mode: a 5-byte `jmp rel32` to a relay within ±2 GiB, written as one atomic qword.
     It relocates 6 bytes on 1.0.2.1 (`push r15; push r14; push r12`).
   - Fallback: a 14-byte absolute jump, which relocates exactly the 14 prologue bytes.
   - Neither mode reaches the host's `call` at +0x15. Undecodable bytes, loop/jrcxz,
     internal branches and out-of-reach rip-relative operands are refused.
   - Patching suspends all other threads, retries while any RIP is inside the patch, saves
     the original bytes, and checks them again before writing. The hook is published only
     after every step succeeded. Init failure and `g6rt_runtime_shutdown` share `teardown()`,
     which restores the bytes.
   - **Late load** (the runtime is injected after the title's threads run; see "Loading under
     DexNom"). `hook.cpp` `write_patch` takes a Toolhelp32 thread snapshot, suspends every other
     thread of the process, and reads each one's RIP. The guarded range is
     `[Run, Run + max(patch length, relocated bytes))`, 6 bytes for the rel32 patch on 1.0.2.1
     (pure check `hook::rip_in_range` / `hook::guarded_len`). A thread inside it, or a live
     thread that cannot be opened, suspended or read (position unknown), makes the round unsafe:
     all threads are resumed, the runtime sleeps 1 ms and tries again. After 200 rounds or
     10 s, whichever comes first, nothing is written and the log says
     `Run hook NOT installed: thread <tid> was inside the patch range <addr>+0x6 (RIP <addr>) in
     all <n> attempts (<ms> ms); nothing written`, followed by `teardown: hook enable failed`.
     On success: `Run hook installed at <addr> (<n> other thread(s) suspended during the write,
     <n> attempt(s) in <ms> ms, guarded range 6 bytes)`. The write itself is unchanged: one
     atomic qword for the 5-byte patch, then `FlushInstructionCache`, then resume.
   - One round walks the system-wide snapshot, which takes ~250 ms with ~6,000 threads on the dev
     machine; the game's threads are suspended only after the walk, for the suspend/check/write.
   - Residual race (not closed): a thread created after the snapshot is not suspended. It would
     have to be between two of Run's first instructions at the instant of the 8-byte atomic
     write to fault. The snapshot is not repeated while threads are suspended because the
     snapshot call may allocate, and a suspended thread may hold the heap lock.
   - The runtime DLL is pinned and the trampoline is never freed, because a thread may still
     be returning through them.
   - The detour forwards all six Run arguments, calls the original first, then:
     - every hooked mode: logs each new script thread (id, name hash at `thread+0x2b8`, state at
       `+0x344`), each distinct name hash the first time it is seen and each new state per name
       hash; samples the game clock every 50 ms; dumps the mount table once (first Run call);
     - full: if `[script] bind` selects this thread (the binder, `src/binder.hpp`: one bound thread at a
       time, chosen under a lock; only state byte 2 or 3, see G-093/G-095), it sets the TLS active-context
       slot to this ctx, evaluates the guard, mounts queued overlays (single-player only), runs
       the fiber scheduler, and restores the slot. Faults turn the hook into a pass-through.
8. **Plugins** (`mode=full`): every `plugins\*.dll` next to the runtime that exports
   `g6_plugin_query`/`g6_plugin_load` with API major 0. Natives are callable only from a
   script fiber during a tick. Any other thread gets `G6_E_NOT_ON_SCRIPT_THREAD` (no queue).
   `vfs_add_overlay` is gated, see "mods/ overlay".

## Game clock (`game_time_ms`)

The runtime resolves the timer block from the manifest handler RVAs of three natives, after
their self-id check, by decoding the handler bodies with the x64 decoder (`game_clock.cpp`).
Plugins reach the same block through `anchor_address("world.game_timer")`: an xref-chain anchor
in `patterns.hpp` along the GET_GAME_TIMER path below (G-016). The self-test checks that both
land on RVA `0xabbef40` of the dump. The handler is walked to its first direct call;
the callee must be exactly `load; ret`.

| native (hash) | handler | value path [observed, 1.0.2.1] |
|---|---|---|
| GET_GAME_TIMER `4F67E8ECA7D3F667` | `0x143e85810` | `call 0x1405f2ef0` = `8B 05` `mov eax,[0x14abbef40]`; `ret`; then `cdqe` |
| GET_FRAME_COUNT `77DFA958FCF100C1` | `0x143e85700` | `call 0x143eb1ef0` = `mov eax,[0x14abbef44]`; `ret` |
| GET_FRAME_TIME `5E72022914CE3C38` | `0x143e85740` | `call 0x140738420` = `C5 FA 10 05` `vmovss xmm0,[0x14abbef4c]`; `ret` |

All three must point into one block (+0, +4, +0xc). The block must lie in a writable,
non-executable section (`.data`, 0xC0000040) and, live, in RW/WC committed pages. A
rip-relative xref sweep finds exactly two writers: an init at `0x142226130` and an updater at
`0x1422260b0`..`0x142226125`. The updater adds the step in ms to +0 and a constant to +4 with one
`vpaddd`, and keeps the previous +0 at +8. So +0 is a u32 millisecond counter that wraps.

At run time the samples go through `clock::Mixer`:
- The game timer is used only after 30 consecutive plausible samples spanning at least 1 s,
  5 of which advanced. A plausible sample is a wrap-safe u32 delta below 2^31 and at most
  4 × the wall delta + 1000 ms.
- Until then, and after more than 8 anomalies, the output follows the system timer (QPC,
  capped at 1 s per update).
- A backward or implausible step adds no time, so the output never goes backwards.
- With the game timer, `wait(ms)` pauses when the game timer pauses.

`[clock] source=system` disables the game timer.

## mods/ overlay (`vfs_add_overlay`, design §5 hook-free path)

**Off by default** (`[vfs] mode=off` returns `G6_E_NOT_IMPLEMENTED`). With `mode=overlay`,
`vfs_add_overlay(root, priority)`:
1. validates `root` as a host directory;
2. maps it to the game's path namespace with `[vfs] path_map`;
3. queues it and returns `G6_OK`, which means accepted, not mounted yet.

On a later script tick, and only while the single-player guard is open, the runtime does,
for each `[vfs] mount_points` entry that already exists in the mount table:
1. constructs a `fiDeviceRelative` in a 0x200-byte slot inside the runtime DLL (never freed);
2. calls `Init(dev, game_root, readOnly=1, parent=null)`;
3. checks `parent->GetAttributes(root) != -1`;
4. calls `MountAs(dev, mount_point)`;
5. reads the table back.

Lower priorities are mounted first, so the highest priority is newest and wins.

ABI facts, all [observed] in 1.0.2.1 and re-checked by `vfs::derive` on every start:

| fact | evidence (VA) | runtime check |
|---|---|---|
| Relative ctor = base ctor + vtable `0x1462fc810` + `[+8]=0` | `0x14165fa50`, base `0x14165fa80` | bytes at vtable-anchor site − 0xd, call target = `fs.device.base.ctor` |
| `Init(this, root, bool r8b, parent r9)`: `[+0x114]`=readOnly, `[+8]`=parent or `GetDevice(fullpath, readOnly)`, root copied to `+0x10` (0x100), one trailing `/` stripped | `0x1428cb210` | instruction-exact stores in the body |
| `MountAs(this, mp) -> bool` = `MountGlobal(mp, this, [+0x114])`, then `[+0x110]=strlen(mp)-1` | `0x141fb2210` | `mov r8b,[rcx+0x114]`, first call = `fs.mount.mount_global`, `[+0x110]` store |
| `MountGlobal(mp, dev, bool)`: mp must end in `/` or `\`, ≤ 0xfd chars; takes lock `0x1478f5ed0`; appends to the first entry whose prefix matches, else creates one | `0x1412c5670` | references `fs.mount.table` and table−4 (count) |
| `sizeof(fiDeviceRelative)` = 0x118 | deleting dtor `0x1457f6c00`: `mov edx,0x118` before `operator delete` | read from vtable slot 0 |
| lookup walks `devices[count-1..0]` calling slot `+0x168` until `!= -1` | `0x1428c8ec0` (`0x1428c8f81..f9e`) | none (behaviour, verify live) |
| `Unmount(mp)` removes the whole entry and frees the array, not the devices | `0x1428cb450` | none |
| the game's own sequence: ctor, `GetDevice(path,1)`, `Init(dev,dir,1,parent)`, `MountAs`, …, `Unmount` | dlclist reader `0x141e19633..0x141e196aa` | none |

**Gated, missing fact:** which string the game's file device (Local, under the ztrx HLE) uses
for a host folder. While `[vfs] path_map` is empty, every request logs
`vfs_add_overlay refused: missing fact: the game-side name of the host folder ...` and returns
`G6_E_NOT_IMPLEMENTED`. If the mapping is wrong, step 3 refuses with
`the game's device reports root '...' as missing`.

While the guard is closed (default: `offline_attested=0`), a configured overlay logs
`vfs overlay deferred: single-player guard closed (see the guard lines)` and is not mounted.

## Identity check (why it is not `sha256(image)`)

A full-image SHA cannot work on the live image, for three reasons:
- the OS relocated the image;
- VIctory maps a *patched copy* (`victory-image-<blake3>.dll`, research/03 §2.1), with the DLL
  bit set, the entry point zeroed, the checksum cleared, the import directory redirected and
  the TLS callbacks zeroed;
- the host rewrites ~53k `gs:[0x58]` loads in `.text`.

The runtime therefore checks fields that none of those steps change:

| check | required | source |
|---|---|---|
| Machine, TimeDateStamp `1794606036`, SizeOfImage `0xb06d000`, the 10-section table (name, RVA, VirtualSize, SizeOfRawData) | yes | manifest + exe header |
| export set = exactly `__XMemAllocationHooks__` @ RVA `0x6844380` | yes | manifest (REA) |
| pefile-compatible imphash of the **original** import descriptors at RVA `0x6791ab4` (the host only repoints the directory) = `eb03ed12…` | one of these (`identity=normal`) | manifest pin; RVA from the exe header |
| SHA-256 of the module's backing file, or of `[runtime] exe_path`, = `e95020b2…a08993` | one of these; the file SHA is required with `identity=strict` | manifest |

The header, export and imphash checks pass on the exe file mapped at its RVAs. The
host-patched fields are logged for information only.

**[assumption]** VIctory does not overwrite the original import descriptors in place. If the
log says `imphash … MISMATCH`, set `exe_path` to the original exe, and the file SHA-256
corroborates instead.

## Configuration keys added in round 2 (`gta6mod_runtime.ini`)

| key | values (default first) | effect |
|---|---|---|
| `[script] bind` | *(empty)*, `stable` (alias `first`), `hash`, `observe` | `stable`: the thread in state 2 or 3 with the longest continuous presence after `bind_observe_ms`; it is replaced when it finishes (state >= 4) or is absent for `rebind_absent_ms` while other threads run. Empty means `hash` if `host_script_hash` is set, else `stable`. `hash` without a hash falls back to `observe`. `observe` never ticks plugin scripts. |
| `[script] bind_observe_ms` | `3000` | Continuous presence a thread needs before it can be bound. |
| `[script] rebind_absent_ms` | `3000` (500..60000) | Absence of the bound thread, while other threads run, that triggers a rebind. |
| `[clock] source` | `auto`, `system` | `auto` uses the game timer once validated. `system` uses QPC only. |
| `[vfs] mode` | `off`, `overlay` | The overlay also needs `[runtime] mode=full` and an open guard. |
| `[guard] offline_attested` | `0`, `1` | Operator attestation that the host blocks the game's network. `0` keeps the guard closed. |
| `[guard] scan_interval_ms` | `5000` (1000..10000) | Interval of the own-process TCP check. A clean scan older than 20 s closes the guard. |
| `[vfs] path_map` | *(empty)* | `<host prefix>\|<game prefix>`. **Unknown until verified live**: empty means refuse. |
| `[vfs] mount_points` | `platform:/,common:/` | Only existing entries are overlaid, at most 4. |
| `[vfs] root` | *(empty)* | Host folder the runtime queues itself at start. |

Expected refusal lines, each naming its cause:
- `vfs_add_overlay: [vfs] mode=off (the default); nothing is mounted`
- `vfs_add_overlay refused: missing fact: the game-side name of the host folder '…'. [vfs] path_map … is empty`
- `vfs_add_overlay refused: engine ABI not established on this build: <which check>`
- `vfs overlay deferred: single-player guard closed (see the guard lines)`
- `refused native <NAME>: [guard] offline_attested is not 1` (or `… a connection to a remote host was seen …`, `no successful connection scan yet`, `last clean connection scan is stale`)
- `guard: CLOSED for this session: N connection(s) to a non-loopback, non-link-local remote, first <addr:port> <STATE> (<class>) …`
- `guard: GetExtendedTcpTable failed (error N); guard closed until a scan succeeds`
- `vfs overlay on 'platform:/' refused: the game's device reports root '…' as missing (GetAttributes == -1): path_map is wrong for this host`
- `game clock: not resolved (<reason>); game_time_ms uses the system timer`
- `game clock: … ; game_time_ms falls back to the system timer for this session`

## First live run (read-only)

1. Run `build.cmd`. Copy `build\gta6mod_runtime.dll` and `build\gta6mod_runtime.ini` to one
   folder that the host process can write to. Leave `mode=readonly`.
2. Load the DLL into the host's title process with `g6inject.exe` (next section). The host
   (DexNom/VIctory) has no plugin or preload mechanism. The injector waits until the title
   image is mapped, so the runtime finds it on its first look.
3. Read `gta6mod_runtime.log`. The lines you should see:
   ```
   game module at 00007FF… size 0xb06d000 (loader list) path '…victory-image-….dll'
   identity: headers match, exports match
   identity: host-patched fields (informational): Characteristics 0x2022 …   (DLL bit, entry 0: expected)
   identity: imphash of original import descriptors (RVA 0x6791ab4): match (eb03ed12…)
   identity: SHA-256 of mapped module file: … (differs) - expected under VIctory, which maps a patched copy
   identity: build 1.0.2.1 accepted (headers + imphash)
   patterns: 442 anchors in … ms: 431 exact by signature, 8 via xref chain (0 chain failures), 0 missing, …   (file-image counts; live may differ)
   required anchors ok: script.exec.run at … (vtable slot 17 agrees), …, resolve_anchor agrees on all four
   natives: 6829 of 6829 handlers verified statically …
   TLS site Run+0x125: E8 xx xx xx xx 0F 1F 40 00          (or 90 90 90 90 after E8: note what you see)
   TLS access: host thunk 00007FF… …
   guard: offline_attested=0; initial connection scan: ok, N own TCP row(s), 0 to a remote
   guard: [guard] offline_attested is not 1: every plugin native call and overlay mount is refused (G6_E_ONLINE_SESSION)
   guard: first connection scan clean (N own TCP row(s), none to a remote); interval 5000 ms
   mode=readonly: Run hook NOT installed, plugins NOT loaded.
   init finished: 0 (ok)
   ```
   These are the counts from the simulated-HLE offline run. Live counts may differ; any
   `NOT FOUND`/`AMBIGUOUS` line names the anchor.
   Round-2 lines in the same run:
   ```
   game clock: GET_GAME_TIMER rva 0x3e85810 -> core 0x5f2ef0; GET_FRAME_COUNT rva 0x3e85700 -> core 0x3eb1ef0;
               GET_FRAME_TIME rva 0x3e85740 -> core 0x738420; block rva 0xabbef40 in .data (writable, not executable); current value … ms
   vfs: engine ABI checks passed (Relative ctor …, Init …, MountAs …, MountGlobal …, sizeof(Relative) 0x118); [vfs] mode=off
   mount table (init): N entries, capacity …        (N may be 0 this early)
   script binding: bind=stable host_script_hash=0x00000000 bind_observe_ms=3000 rebind_absent_ms=3000 gap_ms=2000 (eligible: scrThread state byte 2 or 3)
   ```
4. Next run: set `mode=observe` (and `[script] bind=observe` to be explicit). Get the game to
   a state where scripts run; the menu has none (finding 30). Look for these lines:
   - `Run hook installed`;
   - `script thread first seen: id=… name_hash=0x… state=…`;
   - `script name hash first seen: 0x… state=…` and `script name hash 0x…: new state …`;
   - one `mount table (first Run call): …` block, with one `mount[i] 'prefix' read_only=… devices=…:`
     line per entry, naming each device's kind and the root of any Relative device;
   - `game clock: validated (30 consecutive plausible samples, …)`;
   - the 10-second `Run calls so far: …; game_time_ms=… (game timer; game timer reads …)` summary.

   That answers design §9 question 1 and gives the hash for `bind=hash`.
5. Only then use `mode=full`, with a test plugin in `plugins\`. With `offline_attested=0`
   its native calls return `G6_E_ONLINE_SESSION`. That is the intended default. Set
   `offline_attested=1` only after checking the host's firewall rule (see "Single-player
   guard"); the first tick after a clean scan logs `guard -> OPEN (…)`.

## Loading under DexNom (`g6inject.exe`)

DexNom/VIctory @ `2e01ab4` (read from source, not run): `victory-play.exe` starts a child
`victory.exe run <exe> --execute`, and the title runs in that child. The child maps a patched copy
of the exe with `LoadLibraryExW` as `victory-image-<16 hex of blake3>.dll`
(`crates/victory-loader/src/prepare.rs`), binds its imports, attaches the title DLLs, then runs
title code. The host has no plugin or preload mechanism, so `g6inject.exe` loads the runtime from
outside. Nothing in DexNom is changed.

```bat
rem 1. build\ holds gta6mod_runtime.dll, gta6mod_runtime.ini (mode=readonly) and g6inject.exe
rem 2. start the game with victory-play.exe as usual, then (any time before or after it boots):
sdk\runtime\loader\build\g6inject.exe --dll "C:\full\path\to\build\gta6mod_runtime.dll"
```

| option | default | meaning |
|---|---|---|
| `--dll <path>` | required | absolute path of an x64 DLL; checked (PE, x64, DLL bit) before waiting |
| `--process <name>` | `victory.exe` | process file name to wait for |
| `--pid <n>` | any | inject into this process only; its name must still equal `--process` |
| `--timeout-ms <n>` | `300000` | how long to wait for a matching process (0..86400000) |
| `--image-prefix <p>` | `victory-image-` | a process matches once it has a module `<p>*.dll` |

What it does, each step logged to stdout (errors to stderr, with `ERROR:`):
1. Polls every 250 ms for processes named `--process`. Each one is opened for query only and must
   be x64 (`IsWow64Process2`). A process matches once its module list contains `<prefix>*.dll`.
   A `victory.exe` without the image (for example DexNom's optional audio-capture child, which
   is also `victory.exe`) is listed but never chosen.
2. Two or more matching processes without `--pid`: refuses (exit 4) and lists each pid with its
   image module and path.
3. Opens the target with `PROCESS_CREATE_THREAD | QUERY_INFORMATION | VM_OPERATION | VM_WRITE |
   VM_READ | SYNCHRONIZE`, takes a module snapshot, and **refuses if a module with the DLL's file
   name is already loaded** (exit 7). A second runtime would hook Run twice.
4. `LoadLibraryW` in the target = the target's `kernel32.dll` base + the RVA of `LoadLibraryW` in
   the injector's own kernel32 (logged; normally the same address).
5. `VirtualAllocEx` + `WriteProcessMemory` of the path, `CreateRemoteThread(LoadLibraryW, path)`,
   then waits up to 60 s for that thread or for the process to exit.
6. Frees the path buffer (left allocated, and logged, if the thread did not finish), then takes a
   new module snapshot. Success means the DLL is in the module list: `confirmed: <path> loaded in
   pid <n> at 0x… size 0x…`. The thread exit code is the low 32 bits of the HMODULE and is only
   logged as a hint.

Exit codes (`tools/inject/inject_core.hpp`):

| code | name | when |
|---|---|---|
| 0 | ok | loaded and confirmed by a module snapshot |
| 1 | usage | bad/missing arguments; `--pid` names a process with another name |
| 2 | dll-file | `--dll` missing, unreadable, not a PE file, or not a DLL |
| 3 | timeout | no process with the image module within `--timeout-ms` (the message says whether no process had the name, or none had the module) |
| 4 | ambiguous | several matching processes and no `--pid` |
| 5 | access-denied | the target could not be opened or inspected (run elevated if the game runs elevated) |
| 6 | arch-mismatch | the target process or `--dll` is not x64 |
| 7 | already-loaded | a module with the DLL's file name is already in the target |
| 8 | process-exited | the target exited while waited for or during the load; `--pid` not running |
| 9 | inject-failed | allocation, write or remote thread failed, or the DLL is absent afterwards |
| 10 | internal | local failure (process snapshot, exception) |

**Late load.** The runtime is now loaded after the title's threads are running. What that changes:
- `DllMain` stays minimal (checked, `runtime.cpp` `DllMain`): `DisableThreadLibraryCalls`,
  `GetModuleFileNameW`, `GetEnvironmentVariableW`, `GetModuleHandleExW(PIN)`, `CreateThread`.
  The init thread starts after the loader lock is released, so the remote `LoadLibraryW` returns
  at once (tested: the injector test injects the real runtime into a stand-in process).
- `mode=readonly` writes no code. The only code write in the runtime is
  `InlineHook::write_patch` (`hook.cpp`), reached only through `g_run_hook.enable` in `run_init`
  step 6; `run_init` returns `G6_OK` before step 6 when `mode == readonly`
  (`mode=readonly: Run hook NOT installed, plugins NOT loaded.`). No other source file calls
  `VirtualProtect` or writes into the game image. Readonly does read live game memory (anchors,
  handlers, the clock block, the mount table) without taking the game's locks, so the mount-table
  dump can see a table the game is changing at that moment.
- `mode=observe|full`: the Run patch is protected as described under "Architecture" step 7.

**Verify live (unknown until the first session):**
- that `victory.exe` lets a same-user process open it with the rights in step 3 (no DACL change
  or protection; DexNom's source sets none that we found);
- that `CreateRemoteThread` into the host works while the title runs (DexNom's HLE replaces
  `LoadLibraryExW` only for the title's own imports; the injector calls the real kernel32);
- that `VirtualProtect(PAGE_EXECUTE_READWRITE)` on the host-mapped image succeeds (no
  arbitrary-code-guard policy on `victory.exe`);
- how many threads the title process has, and how long the freeze takes (logged on install).

## Python plugin (`g6_python.dll`)

`sdk/python/host/g6_python.cpp` is an ordinary plugin (query/load/stop/unload, API 0.2) that embeds CPython 3.12
and runs `gta6mod` Python mods; the design, config keys and contracts are in `sdk/docs/python_host.md`.
What it means for the runtime:

- **Off by default.** It reads `plugins\g6_python.ini`; without `[python] enable=1` `g6_plugin_load` logs one
  line, returns `G6_OK` and registers nothing. `python312.dll` is delay-loaded, so it is never loaded then.
- It registers one script, `python-host`. Python starts on that script's first tick (script thread, inside the
  runtime fiber), after a `GET_GAME_TIMER` probe: `G6_E_ONLINE_SESSION` (guard closed) keeps Python from
  starting. Every later native goes through `native_resolve`/`native_invoke` and the guard as usual.
- It never calls `script_wait`: Python mods are generators the host resumes from its own tick, like the Luau
  host. `g6_plugin_stop` runs the mods' stop handlers (natives work there when the runtime calls it from a
  script fiber); `g6_plugin_unload` runs them if the stop event did not.
- Once Python started, `python312.dll` and `g6_python.dll` are pinned and the interpreter is never finalised
  (`Py_FinalizeEx` would join mod threads during shutdown; see python_host.md "Lifetime").
- Offline: `g6_python_test.exe` (17 scenarios, run by `build.cmd test` / `build.ps1 -Test`) loads it against
  `gta6mod_mock.dll`; nothing of it has run in game.

## Design changes from `docs/runtime_design.md`

- **§3 identity probe: not done by calling.** All 6,813 wrappers, after writing their id for
  `argCount<0`, *jump back into the normal body*. For example, SET_ENTITY_COORDS
  `0x143c137f2 jmp 0x143c1376f`, and GET_ENTITY_ALPHA writes `[ret]` at `0x143c09819`, so
  `ret=nullptr` would fault. A probe call with argCount=-1 therefore executes the native. The
  runtime reads the same id statically from the code instead.
- **§3 "5-byte prologue `83 79 10 00 78`":** the cmp is at +4 to +41 after push/sub/vmovaps,
  and a `mov` usually sits between it and the `js` (rel8 in 6,573 handlers, rel32 in 240). The
  check is therefore decoded, not a fixed 5-byte compare.
- **§4 TLS:** the work runs after the original Run returns, so the TLS slot is published by
  the runtime itself.
- **§2 loader model:** the host has no loading path for our DLL; `g6inject.exe` loads it from
  outside after the title image is mapped (late load, see "Loading under DexNom").
- **§6 step 1:** replaced by the identity check above.
- **§6 step 2:** exact-one is required for the four runtime anchors, not for all.
- **§5 timing:** design §2 says "install the overlay at load". The engine path code
  (`FullPath`, the TLS asset-manager slot at TLS+0x7f0/+0x7f8, and the game allocator inside
  MountGlobal) is per-thread, so the mount calls run on the game's script thread inside the
  tick instead, after the game's own mounts. They also wait for the single-player guard.
- **§5 mount points:** a separate Relative device per mount point, because
  `[+0x110]` = strlen(mount point) − 1 is per device. Only existing entries are overlaid.

## Single-player guard

Build 1.0.2.1 has **no** session-state natives. The 16 NETWORK handlers that exist are
constant single-player stubs (finding 26), so no native can tell online from offline. The
guard (design §7) therefore has two inputs, and **both must pass** before any plugin native call
runs or the overlay is mounted:

1. **Attestation:** `[guard] offline_attested=1`. You state that the host blocks the game's
   network access (the ztrx firewall rule). Default `0` keeps the guard closed.
2. **Own-process TCP check** (`guard.cpp`). It is read-only and touches no game code.
   - A worker thread calls `GetExtendedTcpTable` (`TCP_TABLE_OWNER_PID_ALL`, IPv4 and IPv6) every
     `scan_interval_ms` and keeps only rows owned by this process. That is the host process
     that maps the game.
   - Any row in `ESTABLISHED` or `SYN_SENT` whose remote is not loopback (`127/8`, `::1`) or
     link-local (`169.254/16`, `fe80::/10`) **trips the guard for the rest of the session**.
     LAN and other private remotes trip it too. IPv4-mapped IPv6 addresses are classified as IPv4.
   - A failed or stale scan (no clean scan for 20 s) closes the guard until the next clean scan.

A closed guard keeps round-1 behaviour: every plugin native returns `G6_E_ONLINE_SESSION`.
NETWORK-namespace natives are refused regardless. There is no online mode, and no `allow_online`
key or field exists in the runtime or `gta6mod.h`. The old `[guard] session_natives` key and
`kSessionNatives` are removed.

**Verify live:**
- First check, with `offline_attested=0`, that the log shows `initial connection scan: ok` and
  `0 to a remote`.
- Check the host's firewall rule: the game's outbound traffic must be blocked. Note the guard
  only sees this process's TCP sockets. A blocked connect may show briefly as `SYN_SENT` and
  then trip the guard, which is intended.
- Only then set `offline_attested=1`.
- Watch for `guard: CLOSED for this session` during play. If it appears, the network is not
  blocked: do not reopen the guard by editing the ini mid-session.
- Unknown: whether the ztrx host itself opens TCP sockets in this process (for example a
  debugger or telemetry connection). If one shows up as a remote, the guard trips. Report the
  logged remote rather than whitelisting it.

## UI overlay (`g6ui.dll`, `[ui] overlay=1`)

An in-game menu layer for mods: Dear ImGui (v1.91.9b, `sdk/third_party/imgui`) drawn on the host's own
D3D12 swap chain, with the toggle key (default Insert) showing the menu and giving it the mouse and keyboard.
Mods use one C ABI, `g6_ui_api` in `gta6mod.h` ("UI layer"), reached through `g6_host_api.ui` (API 0.3):
C/C++ (`gta6mod/ui.hpp`; `gta6mod/ui_imgui.hpp` to draw with ImGui directly on the shared context), Luau
(`@gta6/ui`, `sdk/luau/types/ui.luau`) and Python (`gta6mod.ui`, backend-agnostic: it calls `backend.ui`; in game
through `g6_python.dll`, whose frame functions run on the script thread and are replayed here by the runtime's
`g6_ui_retained_*` exports, docs/python_host.md "Python UI").
**Nothing of it has run under DexNom.** What is verified is offline (below).

**Design choice: a separate `g6ui.dll` next to the runtime, loaded by the runtime itself (not from `plugins\`).**
The runtime keeps its import table (`KERNEL32`, `bcrypt`, `IPHLPAPI`) and its readonly/observe behaviour exactly
as before: nothing of D3D12, DXGI, user32, d3dcompiler or ImGui is mapped unless `[ui] overlay=1` and
`[runtime] mode=full`. The runtime still owns the gate (ini, mode, single-player guard), loads the overlay
before the plugins (so `g6_plugin_load` can register menus) and stops it before they unload; and the overlay is
testable on its own (`ui_test.exe` loads it into a process with its own swap chain). It uses the runtime's
inline-hook code (`src/hook.cpp`, `src/x64_decode.cpp`, compiled into it), and it pins itself.

Enable: copy `build\g6ui.dll` next to `gta6mod_runtime.dll` and set
```ini
[runtime]
mode=full
[ui]
overlay=1          ; default 0
toggle_key=0x2D    ; virtual-key code (Insert)
queue=auto         ; or ecl
```
Expected log lines (`ui:` source): `entry points (dummy device on default adapter): Present …, ResizeBuffers … in
dxgi.dll; ExecuteCommandLists … in D3D12Core.dll`, three `… hook installed at …` lines, `overlay active`,
`ui: overlay started …`, then on the first Present `input: window … subclassed` and `overlay bound: swap chain …,
window …, <w>x<h> format <n>, <n> buffers, queue … (<source>)`.

How it works:
1. **Entry points.** A dummy D3D12 device (default adapter, else WARP), DIRECT queue and flip-model swap chain on
   a hidden 64x64 window give the vtables: `IDXGISwapChain` slot 8 `Present`, 13 `ResizeBuffers`,
   `ID3D12CommandQueue` slot 10 `ExecuteCommandLists` (COM ABI slots). The targets must lie in `dxgi.dll` and in a
   module named `d3d12*` (D3D12Core.dll here); a vtable pointing elsewhere (another overlay replaced it) is refused.
   The dummy objects and window are destroyed before any hook is written.
2. **Hooks.** Three `hook::InlineHook`s (relocating trampoline, all other threads suspended and checked, original
   bytes saved). They are published (`g_active`) only after all three are written; a failure rolls back the ones
   already written, newest first. `g6ui_stop` (runtime teardown) clears `g_active`, waits for a frame in
   progress, restores the window procedure (only if nobody subclassed after us; otherwise ours stays as a
   pass-through), releases the GPU objects and the ImGui context, and restores the bytes. Trampolines and the
   DLL stay (a thread may still return through them).
3. **Queue.** The overlay must submit on the queue the swap chain presents on (else its draw races the host's).
   `queue=auto` first asks `IDXGISwapChain::GetDevice(ID3D12CommandQueue)`; on Windows 11 build 26300 that query
   returned no queue (both offline runs), so the overlay falls back to the `ExecuteCommandLists` match: the DIRECT
   queues on the swap chain's device that executed since the previous Present (references held while candidates);
   one left wins; several: the one that executed on the presenting thread; otherwise it waits (logs
   `several DIRECT queues …`) and never guesses (`ui_logic.hpp choose_queue`). DexNom @ `2e01ab4` (source read,
   `crates/victory-gpu/src/host.rs`): one DIRECT queue (`create()`), the swap chain made on it
   (`CreateSwapChainForHwnd(&self.queue, …)`, FLIP_DISCARD, 2 buffers) and `present()` executes its copy on that
   queue right before `Present(0, 0)` on the same thread, so exactly one candidate is expected.
4. **Drawing** (on the host's presenting thread, inside our Present detour, before the original Present): bind once
   per swap chain (our RTV heap, one shader-visible SRV slot for the font, 3 allocators, a list, a fence), then per
   frame: wait (≤ 250 ms, else skip the frame) for the allocator slot used 3 frames ago, ImGui new frame
   (DisplaySize = back-buffer size; mouse positions scaled from client to back-buffer pixels), mods' callbacks,
   record barrier PRESENT→RT, draw, RT→PRESENT on the current back buffer (fetched per frame, never held, so
   `ResizeBuffers` needs nothing from us but a wait for our fence), execute on the host queue, signal our fence.
   Nothing is recorded when the menu is hidden and no `G6_UI_FRAME_ALWAYS` frame is registered. A size change is
   followed per frame; a format change re-creates the ImGui DX12 backend; a new swap chain on the same window
   rebinds at once (another window: after the bound one is silent 2 s); a removed device detaches without waiting.
5. **Input.** The swap chain's window (`GetHwnd`) is subclassed (`SetWindowLongPtrW`). Hidden: everything goes to
   the host except the toggle key (swallowed). Shown: mouse and keyboard go to ImGui only (system keys to both, so
   Alt+Tab/Alt+F4 work; `WM_INPUT` swallowed with `DefWindowProc` cleanup); ImGui draws its own cursor. Focus loss
   (`WM_KILLFOCUS`, `WM_ACTIVATEAPP(0)`, `WM_ACTIVATE(WA_INACTIVE)`) and hiding release ImGui's keys/buttons and the
   mouse capture; focus loss also hides the menu.
6. **Mods.** Each frame callback runs inside `PushID`, between `ErrorRecoveryStoreState` and
   `ErrorRecoveryTryToRecoverState` (unbalanced Begin/End/Tree/Tab is repaired and logged) and under SEH (a faulting
   callback is unregistered and logged). Callbacks run only while the single-player guard is open (the overlay's
   status window says why when it is closed); natives are refused there (`G6_E_NOT_ON_SCRIPT_THREAD`: not the
   script thread). A plugin's frames are unregistered with it.

**Verified offline** (`build.ps1 -Test`, `build_ui.cmd test`): `ui_test.exe` 80 checks on the hardware adapter and
80 on WARP: hook rollback after a forced failure of hook 2 and of hook 3 (all bytes back), install, queue bound
via `ExecuteCommandLists` (and via `queue=auto`), WndProc subclassed, nothing drawn while hidden, Insert through
`SendMessage` shows/hides and is not passed on, pixels read back from the back buffer right after the overlay drew
(a plugin's red rectangle at (120,220), the host's blue elsewhere), an ImGui button clicked through `WM_MOUSEMOVE`
/`WM_LBUTTONDOWN`/`UP` while the host sees no mouse message, the host gets input back when hidden, a HUD frame
drawn while hidden, focus loss hides, the gate skips callbacks, error recovery and SEH containment,
`ResizeBuffers` to 800x600 (bottom-right pixel, click with client→back-buffer scaling), to BGRA (backend
re-created, pixel still right), swap chain re-created on the same window, owner unregistration, stop (bytes and
WndProc restored, no detour calls afterwards), restart. A second DLL (`ui_test_plugin.dll`) draws with its own
ImGui copy through `gta6mod/ui_imgui.hpp`. `trainer_mock_test.exe` (18 checks) runs `g6trainer.dll` on the mock.
`loader_selftest` checks the runtime's stub table without the overlay. All of it with MSVC (CMake, `/W4 /WX`)
and with clang-cl (`build.cmd`).

**Verify live** (unknown until the first session; read `gta6mod_runtime.log`):
- that the three entry points resolve and hook inside `victory.exe` (another overlay such as Steam, Discord or
  RTSS may have hooked `Present` first: the log names the module or the undecodable byte), and how long the
  thread freeze takes (logged per hook; 0.05–5.3 s per hook on the dev machine, ~6,000 system threads);
- that DexNom's swap chain is the one bound (`overlay bound: … queue … (ExecuteCommandLists match)`) and no
  `several DIRECT queues` line appears;
- the back-buffer format DexNom presents (`swap_format`; HDR/float formats draw ImGui's colours as linear values);
- frame pacing: the overlay adds one small command list per frame and waits only when its 3-frame ring is busy;
  watch for `frames_skipped` symptoms (menu flicker) at low fps, and compare frame times with the menu shown/hidden;
- input focus: whether DexNom's window (or the one it creates) receives `WM_MOUSEMOVE`/`WM_KEYDOWN` while the game
  runs, and whether the menu's mouse lines up (client→back-buffer scale). **Keyboard keys still reach the game
  while the menu is open:** DexNom's GameInput HLE polls `GetAsyncKeyState` and XInput while its window is
  focused (`crates/victory-hle/src/modules/gameinput.rs`), which window messages cannot block. Mouse buttons are
  not mapped by that HLE (found in source, not observed);
- that mods' menus appear only with the guard open, and that `teardown` (runtime shutdown) restores the window
  procedure and bytes without a hitch.

## Unverified / unknown (live)

- Whether the host's TLS thunk returns `&TlsArray[index]` in `rax` as research/03 describes.
  The runtime calls it as `void** (*)()` on the script thread.
- Whether the import descriptors survive the host's patching (imphash path).
- That Run is the per-slice entry for every script thread, and how often it runs per frame.
  The binding policy is `[script] bind=stable|hash|observe`.
- Fibers on the game thread: FLS is per fiber, and the game CRT's per-thread data on a new
  fiber has not been observed. The thread is converted to a fiber and back each tick.
- Natives: running natives on our tick has not been tried. Faults are caught and the native
  is disabled, but the native mutex may stay held.
- Game clock: that block +0 advances in game ms while playing, stops on pause, and stays
  monotonic across loads. The mixer tolerates resets, but whether a load resets it is
  unobserved. Compare `game timer reads` in two 10-second summaries with the wall time.
- Signatures that pin a `gs:[0x58]` load stop matching under the host. None does since G-091:
  `gen_patterns.py` and the anchor merge reject them, and the `-Dump` self-test resolves every
  anchor again after the DexNom TLS rewrite (`simulated host`, 0 missing required).
- mods/ overlay. Verify these live, in this order:
  1. In an observe-mode dump, find how the game names host files. Look for Relative roots
     in the `mount table` lines, or check which drive/path ztrx maps the install folder to
     (its config or log, or Process Monitor on `victory.exe` showing the CreateFile paths
     while the game reads `common:/data/...`).
  2. Set `path_map=<host prefix>|<game prefix>`.
  3. Check that `platform:/` and `common:/` exist in the dump with ≥ 1 device.
  4. With the guard open (`offline_attested=1`, clean scans), run `mode=full` with `[vfs] mode=overlay` and
     `root=<folder with one known loose file>`. Expect `vfs overlay mounted on 'platform:/':
     root '…' (parent …, attributes 0x…), devices 9 -> 10, newest = ours`.
  5. Confirm the game actually reads the shadow file. Open question: does a later
     `Unmount("platform:/")` (DLC reload, session change) drop the overlay? Does streaming
     bypass the layered lookup? And are RPF9 archive entries shadowed
     at all? They are expected **not** to be: only loose paths go through the lookup.
