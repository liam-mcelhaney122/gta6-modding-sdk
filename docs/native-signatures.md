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
| `v` | `Vector3` parameter | 3 (x, y, z floats) | `g6_vec3` | `Vector3` | `vector` |
| `I` `F` `B` `V` `E` | `int*`, `float*`, `BOOL*`, `Vector3*`, `Entity*`-family out-params | 1 (pointer to scratch) | pointer | appended to the returned tuple | extra return values |
| `n` | `void` (return only) | 0 | `void` | `None` | nothing |

Return codes follow `>`:
- `w`: a returned Vector3 is **one packed 16-byte vector** `{x, y, z, w}`. `x` and `y` are in `ret[0]`, and `z` is in the low half of `ret[1]`.
- `e`: a returned entity is the 16-byte handle in `ret[0..1]`.
- `v`: the legacy 3-slot vector return, kept for compatibility only. Generated GTA VI bindings use `w`.

Example: `GET_ENTITY_COORDS(Entity, BOOL, BOOL) -> Vector3` is `"ebb>w"`. It uses 4 argument slots.

**Entity handles** are 128-bit opaque keys. The game hashes all 16 bytes into a global map, and a `lo` of 0 means
"no entity". Pass a handle back exactly as a native returned it. Don't build one by hand or do arithmetic on it.
`Player` stays a plain int because its slot width is mixed in the handler table. The layout of the `Vector3*`
out-param (`g6_vec3_slots`, 8-byte stride) has not been confirmed for GTA VI.

**Hashes in Luau:** Luau numbers are doubles, so a 64-bit native hash cannot be passed exactly. The generated
Luau code passes each hash to `native.resolve` as a 16-digit hex string, once per native at module load. It then
calls through the resolved handle. Entity handles travel as 16-byte `buffer`s for the same reason.

**Untyped natives:** a native with no usable types gets a generic `u64` wrapper instead of a typed one. It takes
raw slots in and returns the first return slot (`G6_N_<hash>` / `N.RAW` / `N.raw(native, ...)`). When
`argc_inferred` has more slots than the typed parameters fill, the extra slots are emitted as `q` and default to 0.
A 16-byte entity counts as 2 slots and a Vector3 parameter as 3.

**Verification status:** every wrapper carries its native's status in re-db (`verified` / `observed` /
`unverified`). `observed` means the handler was located statically, not that its argument types are proven. Real
backends refuse anything except `verified`, returning `G6_E_UNVERIFIED_NATIVE`, unless the runtime was started
with `allow_unverified`. The offline mock backend accepts everything. When GTA VI has two hashes under one name,
the lower-status (seed) hash keeps the base identifier, and the other gets a hash-fragment suffix.
