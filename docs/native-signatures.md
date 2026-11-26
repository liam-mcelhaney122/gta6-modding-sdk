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
| `u` | `Hash`, `unsigned int` | 1 (zero-extended uint32) | `uint32_t` | `int` (or `str` → joaat) | `number` (or `string` → joaat) |
| `f` | `float` | 1 (IEEE-754 bits in the low 32) | `float` | `float` | `number` |
| `b` | `BOOL`, `bool` | 1 (0/1) | `int` | `bool` | `boolean` |
| `s` | `const char*`, `char*` | 1 (pointer, caller keeps it alive) | `const char*` | `str` / `None` | `string` / `nil` |
| `q` | `uint64_t`, an `argc_inferred` filler slot | 1 (raw u64) | `uint64_t` | `int` (defaults to 0) | `number?` |
| `w` | a vector parameter the handler reads as **one packed 16-byte block** (nativedb `float x, y, z`, or `Vector3`) | **2** (x and y in the first slot, z in the low half of the second) | `g6_vec3` | `Vector3` | `vector` |
| `v` | a `Vector3` parameter with no handler evidence (legacy GTA V form) | 3 (x, y, z floats) | `g6_vec3` | `Vector3` | `vector` |
| `I` `F` `B` `E` | `int*`, `float*`, `BOOL*`, `Entity*`-family out-params | 1 (pointer to scratch) | pointer | appended to the returned tuple | extra return values |
| `V` | `Vector3*` out-param | 1 (pointer to a **16-byte-aligned** `{x, y, z, w}`) | `g6_vec3_packed*` | appended to the returned tuple | extra return value |
| `X` | `Entity*`-family **in/out** param: the handler reads the handle at the pointer and writes it back (evidence per native) | 1 (pointer to a 16-byte buffer filled from the argument) | `g6_entity*` (fill it before the call) | `int` argument; the value read back is appended to the returned tuple | `buffer \| number` argument; `buffer` extra return value |
| `n` | `void` (return only) | 0 | `void` | `None` | nothing |

Return codes follow `>`:
- `w`: a returned Vector3 is **one packed 16-byte vector** `{x, y, z, w}`. `x` and `y` are in `ret[0]`, and `z` is in the low half of `ret[1]`.
- `e`: a returned entity is the 16-byte handle in `ret[0..1]`.
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
that default to 0: `f` where the handler loads a 32-bit float from that slot, `q` otherwise. A 16-byte entity and
a packed vector count as 2 slots, a legacy `v` vector as 3.

**Verification status:** every wrapper carries its native's status in re-db (`verified` / `observed` /
`unverified`). `observed` means the handler was located statically, not that its argument types are proven. Real
backends refuse anything except `verified`, returning `G6_E_UNVERIFIED_NATIVE`, unless the runtime was started
with `allow_unverified`. The offline mock backend accepts everything. When GTA VI has two hashes under one name,
the lower-status (seed) hash keeps the base identifier, and the other gets a hash-fragment suffix.
