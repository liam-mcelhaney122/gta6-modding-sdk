# Native call signatures (shared by C, Python and Luau)

Each generated wrapper describes its native with a short **signature string**,
`"<args>><ret>"`. The C runtime, the Python bindings and the Luau host all marshal
arguments from this same string, so they pack a call into `g6_native_call` slots
(8 bytes each) identically. The GTA VI slot layout differs from GTA V/RDR2. The
evidence is in `findings/phase5/23_handle_abi.md`.

| Code | nativedb types | Slots | C | Python | Luau |
|---|---|---|---|---|---|
| `i` | `int`, `Any`, `Player`, `Cam`, `Blip`, `ScrHandle`, `Interior`, `FireId`, … | 1 (sign-extended int32) | `int32_t` | `int` | `number` |
| `e` | `Entity`, `Ped`, `Vehicle`, `Object`, `Pickup`, `PersChar` | **2** (16-byte opaque handle: lo, hi) | `g6_entity` | `int` (128-bit) | `buffer` (16 bytes; a `number` is accepted as lo) |
| `h` | `ScenarioPoint`: a 16-byte opaque handle that is **not** an entity (typed from handler evidence, G-099) | **2** (lo, hi) | `g6_handle16` | `int` (128-bit) | `buffer` (16 bytes only) |
| `u` | `Hash`, `unsigned int` | 1 (zero-extended uint32) | `uint32_t` | `int` (or `str` → joaat) | `number` (or `string` → joaat) |
| `f` | `float` | 1 (IEEE-754 bits in the low 32) | `float` | `float` | `number` |
| `b` | `BOOL`, `bool` | 1 (0/1) | `int` | `bool` | `boolean` |
| `s` | `const char*`, `char*` | 1 (pointer, caller keeps it alive) | `const char*` | `str` / `None` | `string` / `nil` |
| `S` | `ScriptString`: the handler reads a script **String object**, not a C string (typed from handler evidence, G-120) | 1 (the caller's UTF-8 pointer and its bit in `string_slots`; the runtime hands the native a String built from it) | `const char*` | `str` / `None` | `string` / `nil` |
| `T` | `ScriptText`: the handler reads a script **text object** (`_UILOG_SET_CACHED_OBJECTIVE`; typed from handler evidence, G-120) | 1 (UTF-8 pointer and its bit in `text_slots`; the runtime hands the native a text object built from it) | `const char*` | `str` / `None` | `string` / `nil` |
| `q` | `uint64_t`, an `argc_inferred` filler slot | 1 (raw u64) | `uint64_t` | `int` (defaults to 0) | `number?` |
| `w` | a vector parameter the handler reads as **one packed 16-byte block** (nativedb `float x, y, z`, or `Vector3`) | **2** (x and y in the first slot, z in the low half of the second) | `g6_vec3` | `Vector3` | `vector` |
| `v` | a `Vector3` parameter with no handler evidence (legacy GTA V form) | 3 (x, y, z floats) | `g6_vec3` | `Vector3` | `vector` |
| `I` `F` `B` `E` | `int*`, `float*`, `BOOL*`, `Entity*`-family out-params | 1 (pointer to scratch) | pointer | appended to the returned tuple | extra return values |
| `V` | `Vector3*` out-param | 1 (pointer to a **16-byte-aligned** `{x, y, z, w}`) | `g6_vec3_packed*` | appended to the returned tuple | extra return value |
| `X` | `Entity*`-family **in/out** param: the handler reads the handle at the pointer and writes it back (evidence per native) | 1 (pointer to a 16-byte buffer filled from the argument) | `g6_entity*` (fill it before the call) | `int` argument; the value read back is appended to the returned tuple | `buffer \| number` argument; `buffer` extra return value |
| `n` | `void` (return only) | 0 | `void` | `None` | nothing |

**Script objects (`S`, `T`) are decided per native from evidence** (`tools/phase5/natives_string_params.py`,
`re-db/.../natives/script-vm.string_params.json`, findings/phase5/37_script_strings.md). A slot is `S` when its
qword reaches `test byte [R+0x14], 4` (the String out-of-line flag) and `T` when it is the argument of the text
accessor `0x142055010`, each within two direct calls of the handler. The caller passes text; the runtime builds
the object (`include/gta6mod/script_objects.h`: a String laid out as a string literal after opcode 52, or a text
object whose `+0x18` native object returns the text from its virtual `+0x18`), keeps one per distinct text for
the session, and refuses the call with `G6_E_NO_SUCH_ANCHOR` when the anchors behind it do not check out. The
C++ direct invoker (`natives.hpp`) takes an already built object (`const void*`) and builds nothing.

Return codes follow `>`:
- `w`: a returned Vector3 is **one packed 16-byte vector** `{x, y, z, w}`. `x` and `y` are in `ret[0]`, and `z` is in the low half of `ret[1]`.
- `e`: a returned entity is the 16-byte handle in `ret[0..1]`.
- `h`: a returned scenario point is the 16-byte handle in `ret[0..1]` (16 zero bytes when there is none).
- `v`: the legacy 3-slot vector return, kept for compatibility only. Generated GTA VI bindings use `w`.

Example: `GET_ENTITY_COORDS(Entity, BOOL, BOOL) -> Vector3` is `"ebb>w"`. It uses 4 argument slots.
`SET_ENTITY_COORDS` is `"ewbbbbf>n"` and uses 9: entity in slots 0-1, the packed position in 2-3, the four
BOOLs in 4-7 and a float in slot 8 that nativedb does not list (the handler loads it with `vmovss`).

**Vector parameters are decided per native, from evidence.** nativedb declares no parameter as `Vector3`;
vectors appear as three `float` parameters. `tools/phase5/natives_vector_params.py` walks every
nativedb-typed native with the GTA VI slot widths and looks at the handler's exact argument loads
(`re-db/builds/1.0.2.1/natives/script-vm.vector_params.json`, consumed by `gen_bindings.py`):

- **packed** (`w`): a load of exactly 16 bytes starts at the vector's first slot. 254 float triples in 204
  natives, for example `SET_ENTITY_COORDS` (`vmovups xmm1, [args+0x10]` at `0x143c1378b`), `SET_ENTITY_VELOCITY`,
  `GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS`, `REQUEST_COLLISION_AT_COORD`, `GET_GROUND_Z_AND_NORMAL_FOR_3D_COORD`,
  `SET_CAM_COORD`, `CREATE_CAMERA_WITH_PARAMS`.
- **three float slots** (`fff`): the handler loads each component with `vmovss`. One vector-named triple:
  `TASK_GOTO_ENTITY_OFFSET` (`x, y, z` at `[args+0x28]`, `+0x30`, `+0x38`, handler `0x1444c6710`). Both forms
  exist, so there is no global rule.
- **unconfirmed**: the handler reads do not settle it (no read, an integer read, a 32-byte block copy, or a block
  at another offset), or there is no handler (seed guesses such as `CREATE_VEHICLE`). The nativedb float slots
  are kept, and the generated comment and the API page say "vector layout unconfirmed". 13 natives.

Every generated wrapper also carries `vector_layout` in `sdk/data/generated/natives.index.json`.

**Entity handles** are 128-bit opaque keys. The game hashes all 16 bytes into a global map, and a `lo` of 0 means
"no entity". Pass a handle back exactly as a native returned it. Don't build one by hand or do arithmetic on it.
`Player` stays a plain int because its slot width is mixed in the handler table.

**Scenario points (`h`) are 16-byte non-entity handles, decided per native from evidence.** nativedb types them
`int`, but the GTA VI producers (`_GET_REGISTERED_SCENARIO_POINT`, `CREATE_SCENARIO_POINT_HASH`,
`_GET_SCENARIO_POINT_PED_IS_USING`) return 16 bytes and the scripts pass both slots on. The consumers use the low
qword as the key of the scenario-point map (`[0x14ab5d000]`, lookups `0x14074a300` / `0x141930a50`) and do not
read the high qword; it is carried anyway, so a handle round-trips unchanged. They are not entity handles: the
entity resolvers never see them, and Luau accepts only the 16-byte `buffer` (a `number` cannot hold the key).
`re-db/builds/1.0.2.1/natives/script-vm.param_types.observed.json` (`tools/phase5/natives_param_types.py`) holds the
per-native evidence; natives_build.py applies it and refuses stale records. Example:
`_GET_SCENARIO_POINT_COORDS` is `"hb>w"`, `TASK_USE_SCENARIO_POINT` is `"ehsibbubfbq>n"`. No out-pointer form
exists (no native needs one).

**nativedb strings that GTA VI takes as a hash (G-114).** Where the handler reads a `const char*` parameter's slot
32 bits wide (a pointer needs 64) and the value reaches a 32-bit key compare, the parameter is typed `Hash` (`u`),
from the same evidence file. Example: `SET_TIMECYCLE_MODIFIER` is `"u>n"`. Python and Luau still accept the name
as a string and pass its joaat. The trace walks every path of the handler and its callees (up to 7 calls deep),
follows a value passed by the address of a stack cell, and keeps it across `bts` of a high bit and `cmov`
(G-117): 110 parameters are typed `Hash` this way, for example every scripted-conversation `convoRoot`, the
DECOR_* and ped-blackboard names and the anim dictionary/clip names. Natives where the slot is read 32 bits wide
but no compare was found keep `s` and are listed as doubtful in that file's `_meta.doubtful`, each with a
hand-traced `consumer_trace` (`findings/phase5/34_natives_round2.md`). A hand-traced consumer retypes only when
its anchors decode as claimed (the slot load is a 32-bit read of the slot, the compare a 32-bit `cmp`, an integer
use a 32-bit load; record field `traced: true`): `SET_PARTICLE_FX_OVERRIDE` (both asset keys) and
`TASK_START_SCENARIO_AT_POSITION` `p9` are typed `Hash` this way, `_0xCA0BAC376C541978` `p0` is typed `int` (the game
prints it with `%d`) (`findings/phase5/36_native_prototypes.md`).

**Parameter lists that differ from nativedb (G-118).** When every script call site passes the same number of slots
and the nativedb list does not fit it, `tools/phase5/natives_prototypes.py` looks for the one order-preserving
subset of the nativedb parameters that fills those slots in a way the handler reads allow (a float in a float
slot, a string in an integer slot that no script fills with a small constant and that is not dereferenced as a
struct, an entity in a 16-byte block, ...). A slot the handler dereferences as a struct, where no nativedb
parameter fits, becomes a raw `uint64_t` parameter `ref<i>` (the dropped nativedb parameters are fields of that
struct; the SDK cannot build one). The result is in `re-db/builds/1.0.2.1/natives/script-vm.prototypes.observed.json`;
`natives_build.py` applies it before the retypes above and refuses a record whose nativedb list, handler reads or
slot count changed. Examples: `SET_PED_USING_ACTION_MODE` is `"ebi>n"` (nativedb's `action` is not taken),
`START_SCRIPT_FIRE` is `"wfsfi>i"` (the string is the slot-3 script string object; nativedb's `p3` and `p5` are
not taken), `TASK_ENTER_ANIM_SCENE` is `"eq>n"` (one struct reference after the ped). Natives where several or no
layouts fit are listed in that file's `_meta.sweep` and keep the nativedb list.

A typed `uint64_t` parameter (code `q` without padding, e.g. the texture arguments of `_DRAW_SPRITE_WITH_UV`) is
passed as a raw 64-bit slot and has no default.

**`Vector3*` out-params** receive one 16-byte `{x, y, z, w}` written with `vmovaps`, which faults on an address
that is not 16-byte aligned. Seen in the command bodies of GET_VEHICLE_NODE_POSITION (`0x1420f3356`),
GET_MODEL_DIMENSIONS (`0x1420a1760`, `0x1420a1769`), GET_SHAPE_TEST_RESULT (`0x141aba22c`, `0x141aba236`),
_GET_VOLUME_BOUNDS, WAYPOINT_RECORDING_GET_COORD, GET_CLOSEST_FIRE_POS and GET_CLOSEST_ROAD; no Vector3* native
was seen writing the GTA V 8-byte-stride layout. C callers pass a `g6_vec3_packed` (declared 16-byte aligned); the
Python marshaller and the Luau host allocate aligned scratch. `g6_vec3_slots` is kept only for source compatibility.

**Hashes in Luau:** Luau numbers are doubles, so a 64-bit native hash cannot be passed exactly. The generated
Luau code passes each hash to `native.resolve` as a 16-digit hex string, once per native at module load. It then
calls through the resolved handle. Entity handles travel as 16-byte `buffer`s for the same reason.

**In/out entity pointers (`X`) are decided per native, from evidence.** nativedb types `DELETE_ENTITY(Entity*)` and
`SET_ENTITY_AS_NO_LONGER_NEEDED(Entity*)` like out-params, but the GTA VI command bodies read the 16-byte handle at the
pointer (`qword [p]`, `[p+8]`) and store 16 zero bytes back (`vmovups xmmword [p], xmm0`) when the entity is gone or
released. `re-db/builds/1.0.2.1/natives/script-vm.inout_params.json` lists the eight natives where this was read from
the handler (DELETE_ENTITY/PED/VEHICLE/OBJECT, SET_ENTITY/PED/VEHICLE/OBJECT_AS_NO_LONGER_NEEDED; addresses in
`findings/phase5/30_script_layer_evidence.md`). `gen_bindings.py` turns exactly those `E` params into `X` and
refuses stale evidence. Every other `Entity*` stays `E` (out-only). Example: `DELETE_ENTITY` is `"X>n"`; in Python
`(handle,) = N.ENTITY.DELETE_ENTITY(handle)` returns `0` once the game cleared it.

**Untyped natives:** a native with no usable types gets a generic `u64` wrapper instead of a typed one. It takes
raw slots in and returns the first return slot (`G6_N_<hash>` / `N.RAW` / `N.raw(native, ...)`). When
`argc_inferred` has more slots than the typed parameters fill, the extra slots are emitted as padding parameters
that default to 0: `f` where the handler loads a 32-bit float from that slot, `q` otherwise. A 16-byte entity, a
16-byte scenario point and a packed vector count as 2 slots, a legacy `v` vector as 3.

**Verification status:** every wrapper carries its native's status in re-db (`verified` / `observed` /
`unverified`). `observed` means the handler was located statically, not that its argument types are proven. Real
backends refuse anything except `verified`, returning `G6_E_UNVERIFIED_NATIVE`, unless the runtime was started
with `allow_unverified`. The offline mock backend accepts everything. When GTA VI has two hashes under one name,
the lower-status (seed) hash keeps the base identifier, and the other gets a hash-fragment suffix.
