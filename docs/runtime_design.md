# Runtime design (Phase 6, not built yet)

Scope: an in-process mod loader for single-player GTA VI, build 1.0.2.1 (Xbox Series dump,
`game_scarlett_master.exe`, SHA-256 `e95020b2…a08993`). This document is the contract the
runtime will implement; the SDK bindings in `sdk/include/gta6mod/` are already generated
from `re-db` for it. Nothing here was run against the game.

Evidence conventions: **[observed]** from bytes/REA, **[inferred]** reasoned, **[unknown]**.

---

## 1. Ground rules

1. **Never replace a native handler.** GTA VI handlers self-identify when called with a
   negative `argCount` (`findings/phase2/13_script_vm.md` §4), and it is **[unknown]** whether
   anyone verifies that. Call handlers, do not patch them. Trampolines are only used for the
   single script-thread dispatch function and (optionally) the mount lookup.
2. **Signatures, not addresses.** Every engine entry point comes from
   `sdk/include/gta6mod/patterns.hpp` / `sdk/data/generated/patterns.1.0.2.1.json`, resolved
   with `g6::patterns::find` + `deref`. RVAs in the manifest are a cache and a cross-check.
3. **Refuse unknown builds.** See §6.
4. **Single-player only.** See §7.
5. No game bytes, keys or assets are shipped. Only code signatures and offsets.

---

## 2. Loader model

**PC release (future).** The runtime is a DLL with a stable C ABI (`sdk/include/gta6mod/gta6mod.h`),
loaded by:
- a `dinput8.dll` proxy next to the exe (survives without touching the image), or
- `plugins/*.asi` scanning from a small bootstrap, which is the ScriptHookV convention.

**Console-emulator host (now-ish).** A VIctory-style host already loads this exe as a patched DLL
and HLEs the Xbox imports (`research/03`). Our DLL goes in through the host's normal
`LoadLibraryExW` path, after the host has finished rewriting `gs:[0x58]` accesses. No
console-specific code is added to the SDK.

**Load order** **[design]**:
1. Read the image hash and PE headers from `GetModuleHandleW(nullptr)`; do **not** run anything
   before the header check passes.
2. Resolve every pattern in one pass. Require exact-one matches (§6).
3. Register the plugin ABI (`g6_get_host_api`) before any mod is allowed to call natives.
4. Install the script-thread hook (§4) and, if the mount path is used, the `mods/` overlay (§5).
5. Only then call `g6_plugin_load` for plugins.

**Relocation** **[observed]:** the image has `dynamic_base` and `high_entropy_va` set, so the
mapping base differs every run. All pattern results are RVAs added to that base. ASLR is also why
`address_manifest` stores RVAs and patterns, never absolute addresses.

**Patching safety** **[observed, REA `ev_7723239f…`]:** `guard_cf` is disabled,
`cfg_instrumented` disabled, `force_integrity` disabled, no Authenticode directory. CFG is not a
concern for trampolines, and there is no per-page integrity check declared. (Whether the game
re-hashes code at runtime is **[unknown]**; that is a further argument for not editing handlers.)

**TLS** **[observed, REA `ev_e6b8e63a…`]:** two TLS callbacks (`0x1455dd4f4`, `0x1455e2ebc`),
TLS index at `0x146e08b2c`, TLS block `0x1480` bytes. The script VM publishes the active
`scrExecContext` at `tls+0x1478` (`findings/phase2/10_mem_tls.md`, `…/13_script_vm.md`). Any native
call that needs thread context must run with this slot pointing at a valid context — which is
exactly what running inside the tick hook guarantees (§4).

---

## 3. Native invoker

**ABI** **[observed, `types/parts/script-vm.json`]**: `void handler(scrNativeCallContext*)` where

```
+0x00  u64* args
+0x08  u64* ret
+0x10  s32  argCount
+0x14  u32  retCount
```

This differs from GTA V/RDR2 (`{ret@0, argc@8, args@0x10}`); the generated `g6_native_ctx`
(`sdk/include/gta6mod/natives.h`) matches GTA VI, not the older shape.

**Resolution.** Two paths, in preference order:

1. **Direct handler RVA from re-db** — `natives.json` → `natives.index.json` →
   `address_manifest.natives[hash].handler_rva`. 6,813 handlers are located
   (`findings/phase2/13_script_vm.md`), so this covers the whole table on a known build.
2. **The game's own lookup**, if a title update changes handler layout: the script VM resolves a
   native operand to `base + operand`, and `base` is per-program. Until a public, stable resolver
   entry is identified this path stays **[inferred]**; on 1.0.2.1 path 1 is sufficient.

**Handler sanity check** **[observed, corrected 2026-11-25]:** every wrapper contains
`cmp dword [rcx+0x10], 0` followed by a `js`, but **not at a fixed position**: the `cmp` sits 0 to 0x40 bytes
into the function, after the prologue (`push`/`sub rsp`), and a `mov` can sit between it and the `js`
(for example `SET_ENTITY_COORDS` `0x143c13766`/`0x143c1376d`). The runtime decodes the head to find the pair,
so it does not compare five fixed bytes.

**Static identity check (replaces the call probe).** The `js` branch target holds
`mov rax,[ctx]; movabs rcx, <id>; mov [rax], rcx` and then **jumps back into the normal native body**
(`SET_ENTITY_COORDS`: id write `0x143c137ef`, `jmp 0x143c1376f`; `GET_ENTITY_ALPHA`: `jmp 0x143c097ef`).
A call with `argCount = -1` therefore is **not** read-only. The native runs with `args[0]` overwritten by
the id, and a getter then writes through `ret` (null in the old probe design, so it would crash at
`0x143c09819`). **Never call a handler to identify it.** Instead the runtime reads the `movabs` immediate
from the code and requires it to equal `~rotl64(hash, 32)`. On the file, all 6,813 handlers pass this check
(`sdk/runtime/loader`). A mismatch drops the native to `unverified` (`G6_E_UNVERIFIED_NATIVE`).

**Threading.** Natives run on the game's script thread, inside the hook (§4). Calls from a foreign
thread are queued and complete when the hook next runs; until then the API returns
`G6_E_NOT_ON_SCRIPT_THREAD`. `scrNativeCallContext` has **no vector scratch and no dataCount**
field in GTA VI, so callers never get a GTA V–style vector fixup.

**Vectors** **[observed, corrected 2026-11-25]:** an earlier version of this section said `Vector3` returns
are three plain float slots. They are not. A `Vector3` return is one packed 16-byte `{x, y, z, w}` in
`ret[0..1]`, and a `Vector3` *parameter* is usually the same packed block in two argument slots
(`SET_ENTITY_COORDS` `0x143c13760` loads `[args+0x10]` with one `vmovups`). This is decided per native
from the handler's argument loads, because one native (`TASK_GOTO_ENTITY_OFFSET`) reads three float slots.
`Vector3*` out-params get one 16-byte `vmovaps` store, so their buffers must be 16-byte aligned. The
signature string carries all of this (`w`, `fff`, `V`; `sdk/docs/native-signatures.md`,
`findings/phase5/23_handle_abi.md`). The runtime itself passes slots through unchanged and does not encode
the vector layout.

**Limits.** `argCount` is an `s32`: 32 slots max in the C ABI. `retCount` is advisory; the C ABI
keeps 4 return slots, which covers a packed `Vector3` (2 slots) or an entity handle (2 slots) plus
two scalars.

---

## 4. Script-thread tick hook

**Hook point** **[observed, high]:** `scrExecContext::Run`, `0x143de1f10` (`script.exec.run`,
vtable slot 17 / `+0x88` of the exec context vtable). It is called indirectly only, so a vtable
entry patch or a detour at the function start are both viable; prefer the function detour and keep
the vtable intact.

What `Run` does: saves `tls+0x1478`, installs its own context (`ctx+8` = thread), calls the
interpreter, restores the slot. A detour therefore inherits a correct TLS context for free.

**Model** **[design]**:
- On entry, if `[thread+0x2b8]` (script name hash) matches the registered script's hash and state
  `[thread+0x344] == 1`, run one tick of every plugin script bound to it.
- Call the original `Run` first for game scripts, or last for ours; either is fine as long as one
  `Run` call wraps our work so the TLS slot is valid for natives.
- Fibers: one fiber per plugin script, resumed inside the hook, yielding on `script_wait`. Use
  `CreateFiber`/`SwitchToFiber` and never block the game thread — no `Sleep`, no synchronous I/O
  on a script fiber.
- Dedicated-thread model: a worker thread queues natives and blocks on the next hook; simplest
  correct fallback when a plugin resists fibers.

**Why not hook the interpreter (`0x143de2550`) or `Update` (`0x143de1000`)?** They are hotter and
run per opcode / per thread state change. `Run` runs once per script execution slice and owns the
TLS publish, which is the property we need. `script.thread.update` (`vtable slot 3`) stays as a
secondary option only **[inferred]**.

**Scheduling.** No reliance on frame rate for time: `game_time_ms` reads the game clock, so
`wait(ms)` in Luau/Python is expressed in game time and is frame-rate independent. A tick that
overruns is skipped, not queued.

**Why `script_wait` is host-side (2026-11-25, `findings/phase5/26_builtin_path.md` round 2)** **[observed]:**
GTA VI scripts have no WAIT native and no yield opcode. The only writes of `ctx+0x10 = 2` ("returned") are the
outermost return `0x143de7d62` and `scrExecContext::Run` `0x143de20bb`, so each Run executes a script function
to completion. Game scripts wait through `async`/`await` on the built-in `Task` type: `get_IsCompleted` and
`get_CompletedTask` are common in the story programs, and the completion builtin `0x140dd2ce0` sets the task
status and schedules the continuations of the owning thread through `0x140dd2dd0` → `0x142098fb0`. Plugin
scripts therefore keep the fiber model above. A fiber yields back to the Run detour, which resumes it when
the game clock `0x14abbef40` reaches its deadline **[inferred, medium]**. Plugin code never tries to suspend a
game script.

---

## 5. `mods/` overlay (file device layer)

**Preferred: no hook at all** **[observed, high value, `findings/phase2/11_fs.md` §1,§6].** The
device manager already keeps a per-prefix device list and walks it **newest → oldest**, calling
`GetAttributes` (vt `+0x168`) until one reports the file exists. `MountGlobal` (`0x1412c5670`)
appends a device to an existing prefix. So:

1. Create a Relative device rooted at `<game>/mods/` (`fs.device.relative.init` / `mount_as`,
   pattern of the `dlclist.xml` reader).
2. `MountGlobal` it on `platform:` and `common:` **after** the game's own mounts, last in the list.
3. Newest-wins does the shadowing; the engine's packfiles are untouched.

Prefixes seen in code: `platform:`, `common:`, `platformcrc:`, `commoncrc:`, `packcache:`,
`memory:`, `growbuffer:`, `tcpip:`, `globalroot` **[observed]**. `mods/` should cover the first
four; `packcache:` only matters for writable caches.

**Fallback hook** (if a title update changes the layering): detour `fs.mount.lookup`
(`0x1428c8ec0`) and `fs.device.get_device` (`0x140968490`) to try `mods/` first, or the device
slots `GetAttributes +0x168` / `OpenBulk +0x28` / `GetFileSize +0xf8` / `ReadBulk +0x60`. This is
the OpenIV/RageOpenV model. Slot offsets are from the recovered `fiDevice` vtable
(`types/parts/fs.json`, base `0x1462fcaa0`).

**Archive entries.** File-level shadowing covers loose files. Replacing a file *inside* an RPF9
needs packfile entry-lookup (vt `+0x2a8`) redirection and stays
**[inferred]** until an unencrypted-archive path is accepted. RPF9 decoding is done offline
(by private research tooling), but the runtime does not use it: it only ever sees the engine's
already-mounted view.

---

## 6. Versioning and build refusal

At load:

1. `sha256(image)` must equal a key in `sdk/data/generated/address_manifest.<build>.json`.
   Unknown → fail with `G6_E_UNSUPPORTED_BUILD`, load nothing.
2. Every pattern in `patterns.hpp` must match **exactly once**. One miss or ambiguity → fail the
   same way, and name the offending anchor in the log.
3. Every resolved RVA is compared to the manifest value; a mismatch is a warning, not fatal (the
   pattern is the authority, the RVA the cross-check).
4. Handler RVAs are checked statically with §3 (code-embedded id), never by calling them.

`python -m gta6mod patterns check --exe <exe>` is the offline form of step 2 and is the CI gate for
a new build. It exits non-zero unless every pattern matches once and resolves to its recorded RVA.
Anchors without a unique TLS-free signature of their own resolve through **xref chains**: a
signed start (a caller or referencing instruction) plus steps such as `call_target`, `rip_target`, `add`,
`nth_call` and `expect` (`findings/phase5/22_generators.md`). On 1.0.2.1 these are `pool.buildings`,
`script.thread.current_id`, `tls.alloc_current.read`, `tls.thread_id_cached`,
`ecs.ped.comp_weapon_26` and two offline-decoder internals. Their `table` row has an empty signature,
so the runtime must resolve every anchor through `g6::patterns::resolve_anchor`, not `find` alone.
Offline-decoder anchors (archive decoding internals) are not part of the public SDK; the runtime never
uses them. The README lists the current public totals.

Minimum extra manifest pin: `imports.imphash_md5` (`eb03ed12c66f4960e523f3c2c37b3970` on
1.0.2.1) and the export set (only `__XMemAllocationHooks__`) are cheap secondary identity checks
if the SHA ever needs corroboration.

---

## 7. Single-player guard

The exe **is** `GTAO`, so online code is always present. The runtime runs plugins only when the game
is offline.

**Why not a native (revised 2026-11-25, `findings/phase5/26_builtin_path.md`)** **[observed]**: build
1.0.2.1 has **no** session-state natives. NETWORK_IS_SESSION_STARTED, _IN_SESSION, _SESSION_ACTIVE,
_GAME_IN_PROGRESS, _SIGNED_ONLINE and about 30 related names have no handler. The 16 NETWORK handlers that
exist are constant stubs (IS_HOST_OF_THIS_SCRIPT = 1, THIS_SCRIPT_IS_NETWORK_SCRIPT = 0, owner `0xff`), so
they always give the single-player answer and prove nothing. Reading the game's online-service object
would mean analysing online code, which this project does not do.

**Guard design**:
1. **Operator attestation.** `[guard] offline_attested=1` in the ini states that the host blocks the game's
   network access (the ztrx firewall rule). Without it, plugin natives are refused with
   `G6_E_ONLINE_SESSION`.
2. **Process connection check.** This is read-only and touches no game code. A worker thread lists the
   process's TCP connections (`GetExtendedTcpTable`, owner PID = own process) every few seconds.
   - Any connection to a non-loopback, non-link-local address in `ESTABLISHED` or `SYN_SENT` state
     **closes the guard**, with a log line giving the remote address and state. Loopback (`127.0.0.0/8`, `::1`) is ignored.
   - The guard stays closed for the rest of the session, so it does not re-open on its own.
3. Both checks must pass before any plugin native call runs or the `mods/` overlay is mounted.
   `allow_online` is removed: there is no supported online mode.

No networking, matchmaking, anti-cheat, entitlement or integrity code is touched. A future build that
restores real session natives can add them as a third input; it must not replace check 2.

---

## 8. Public API shape

Three layers, deliberately separate (plan §3 Phase 6):

| Layer | Content | Lifetime |
|---|---|---|
| platform | loader, pattern resolver, tick hook, VFS | per game build, rewritten here |
| bindings | `natives.h`/`natives.hpp`, `offsets.hpp`/`classes.hpp`, manifest | regenerated from `re-db`, per build |
| public API | `gta6mod.h` (C ABI), Python, Luau | stable across builds |

Plugins only see the public API plus generated names. `anchor_address()` exposes raw anchors to
advanced plugins without letting them hard-code an RVA.

---

## 9. Open questions

1. Who calls `scrExecContext::Run` (the per-frame thread iterator) — needed to know whether the
   hook fires for every script or only the script VM's own dispatch **[unknown]**.
2. Is the negative-`argCount` path an integrity check that also inspects the *wrapper bytes*? If
   yes, even a 5-byte detour on a handler could be noticed. Because that path falls through into the
   native body, whatever calls it must pass harmless arguments; no caller has been found yet.
3. In-game confirmation of `mods/` newest-wins ordering (the static evidence is strong, the
   runtime behaviour is not observed).
4. Whether a PC build keeps the same `fiDevice` slot order and the same TLS layout.
