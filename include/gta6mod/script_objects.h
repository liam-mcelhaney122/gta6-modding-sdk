/*
 * gta6mod/script_objects.h — the two script object types natives take as one 8-byte slot (G-120).
 *
 * Evidence: findings/phase5/37_script_strings.md (build 1.0.2.1). Header-only C (C11 / C++17). The runtime
 * builds these objects for g6_native_call.string_slots / text_slots; the mocks build the same layouts, so
 * offline handlers read them exactly as the game's handlers do.
 *
 * Script String (signature code 'S', engine type 6). What opcode 52 hands a native for a string literal:
 *   +0x00  const void*  type record        (anchor script.string.type_record; opcode 52 writes it)
 *   +0x08  int32        length in bytes, without the NUL (String.get_Length 0x143dfdb80)
 *   +0x0c  uint32       capacity = length + 1
 *   +0x10  uint32       element stride = 1 (one byte per character: get_Chars 0x143e010bd movsx byte)
 *   +0x14  uint8        flags; bit 2 = the text is out of line and +0x28 holds a pointer to it
 *   +0x18  uint64       9 in every string literal (role unknown; run-time strings get the element
 *                       record's +8 there from the array allocator 0x1451a4400)
 *   +0x20  const void*  element record     (anchor script.string.literal_elem_record; opcode 52 writes it)
 *   +0x28  char[]       the bytes, then a NUL
 * Readers: `test byte [s+0x14], 4` then the text at s+0x28 or through the pointer there (START_SCRIPT_FIRE
 * 0x143c170ce, get_Chars 0x143e010b0). A null String reaches handlers as 0.
 *
 * Script text object (signature code 'T', the localized text class 0x148fdc8f8). Every native reads it only
 * through 0x142055010: null -> ""; otherwise the native text object at +0x18, whose virtual +0x18 is called
 * with (this, true) and returns the const char* (0x14100b8b0: mov rax,[rcx]; mov dl,1; jmp [rax+0x18]).
 * The SDK object below provides exactly those reads: +0x18 -> {vtable, text}, vtable slot 3 -> text. It is
 * not a game-made text object (scripts make those from a text-label hash); whether every consumer is content
 * with these reads is checked per native by tools/phase5/natives_string_params.py (the accessor is the only
 * reader of the slot) and must still be confirmed live.
 */
#ifndef GTA6MOD_SCRIPT_OBJECTS_H
#define GTA6MOD_SCRIPT_OBJECTS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define G6_SSTR_TYPE         0x00
#define G6_SSTR_LENGTH       0x08
#define G6_SSTR_CAPACITY     0x0c
#define G6_SSTR_STRIDE       0x10
#define G6_SSTR_FLAGS        0x14
#define G6_SSTR_Q18          0x18
#define G6_SSTR_ELEM         0x20
#define G6_SSTR_TEXT         0x28
#define G6_SSTR_OUT_OF_LINE  0x04u
#define G6_SSTR_LITERAL_Q18  9u
#define G6_SSTR_MAX_LENGTH   0x100000u  /* SDK limit (1 MiB), not a game limit */

#define G6_STEXT_NATIVE      0x18       /* text object +0x18: native text object */
#define G6_STEXT_SIZE        0x20       /* bytes of the wrapper the SDK writes (+0x00..+0x1f) */

/* Bytes a String of `len` text bytes needs (8-byte multiple). */
static inline size_t g6_script_string_bytes(size_t len) { return (G6_SSTR_TEXT + len + 1u + 7u) & ~(size_t)7u; }

/* Writes a String with inline text into `mem` (8-byte aligned, g6_script_string_bytes(len) bytes), laid out as
   a string literal after opcode 52 ran. Returns mem. */
static inline void* g6_script_string_init(void* mem, const char* text, size_t len, const void* type_record,
                                          const void* elem_record) {
    uint8_t* p = (uint8_t*)mem;
    const uint32_t n = (uint32_t)len;
    const uint32_t cap = n + 1u, stride = 1u;
    const uint64_t q18 = G6_SSTR_LITERAL_Q18;
    memset(p, 0, G6_SSTR_TEXT);
    memcpy(p + G6_SSTR_TYPE, &type_record, sizeof type_record);
    memcpy(p + G6_SSTR_LENGTH, &n, 4);
    memcpy(p + G6_SSTR_CAPACITY, &cap, 4);
    memcpy(p + G6_SSTR_STRIDE, &stride, 4);
    memcpy(p + G6_SSTR_Q18, &q18, 8);
    memcpy(p + G6_SSTR_ELEM, &elem_record, sizeof elem_record);
    if (n) memcpy(p + G6_SSTR_TEXT, text, n);
    p[G6_SSTR_TEXT + n] = 0;
    return mem;
}

/* The text of a String the way the game's readers take it (null object -> NULL). */
static inline const char* g6_script_string_text(const void* obj) {
    const uint8_t* p = (const uint8_t*)obj;
    const char* out;
    if (!p) return NULL;
    if (p[G6_SSTR_FLAGS] & G6_SSTR_OUT_OF_LINE) { memcpy(&out, p + G6_SSTR_TEXT, sizeof out); return out; }
    return (const char*)(p + G6_SSTR_TEXT);
}

static inline int32_t g6_script_string_length(const void* obj) {
    int32_t n = 0;
    if (obj) memcpy(&n, (const uint8_t*)obj + G6_SSTR_LENGTH, 4);
    return n;
}

/* ---- text objects --------------------------------------------------------------------------------- */
typedef struct g6_script_text_native g6_script_text_native;
/* Virtual +0x18 of the native text object: (this, true) -> const char* (x64: rcx, dl -> rax). */
typedef const char* (*g6_script_text_get_fn)(const g6_script_text_native* self, unsigned char flag);
typedef struct g6_script_text_vtable {
    void* slot0;
    void* slot1;
    void* slot2;
    g6_script_text_get_fn get;          /* +0x18 */
} g6_script_text_vtable;
struct g6_script_text_native {
    const g6_script_text_vtable* vtable;  /* +0x00 */
    const char* text;                     /* +0x08 (SDK field; the game only calls vtable->get) */
};

static inline const char* g6_script_text_native_get(const g6_script_text_native* self, unsigned char flag) {
    (void)flag;
    return self ? self->text : NULL;
}

/* The vtable an SDK text object uses: only slot 3 (+0x18) is set. Keep one instance for the process. */
static inline void g6_script_text_vtable_init(g6_script_text_vtable* vt) {
    memset(vt, 0, sizeof *vt);
    vt->get = g6_script_text_native_get;
}

/* Bytes a text object with `len` text bytes needs: wrapper, native object, text + NUL (8-byte multiple). */
static inline size_t g6_script_text_bytes(size_t len) {
    return (G6_STEXT_SIZE + sizeof(g6_script_text_native) + len + 1u + 7u) & ~(size_t)7u;
}

/* Writes a text object into `mem` (8-byte aligned, g6_script_text_bytes(len) bytes). Returns mem. */
static inline void* g6_script_text_init(void* mem, const char* text, size_t len, const g6_script_text_vtable* vt) {
    uint8_t* p = (uint8_t*)mem;
    g6_script_text_native* nat = (g6_script_text_native*)(p + G6_STEXT_SIZE);
    char* t = (char*)(p + G6_STEXT_SIZE + sizeof(g6_script_text_native));
    memset(p, 0, G6_STEXT_SIZE);
    if (len) memcpy(t, text, len);
    t[len] = 0;
    nat->vtable = vt;
    nat->text = t;
    memcpy(p + G6_STEXT_NATIVE, &nat, sizeof nat);
    return mem;
}

/* The text of a text object the way 0x142055010 reads it: null object -> "", null native object -> NULL. */
static inline const char* g6_script_text_read(const void* obj) {
    const g6_script_text_native* nat;
    if (!obj) return "";
    memcpy(&nat, (const uint8_t*)obj + G6_STEXT_NATIVE, sizeof nat);
    if (!nat) return NULL;
    return nat->vtable->get(nat, 1);
}

#ifdef __cplusplus
}
#endif

#endif /* GTA6MOD_SCRIPT_OBJECTS_H */
