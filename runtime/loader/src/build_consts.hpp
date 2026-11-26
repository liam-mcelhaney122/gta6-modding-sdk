// Every build-dependent value the runtime uses that is NOT a pattern or a manifest RVA.
// All are offsets relative to an object/function, valid for 1.0.2.1 only.
// Provenance labels: [observed] = read from the 1.0.2.1 file bytes; [finding] = project report.
#pragma once
#include <cstdint>

namespace g6rt::build {

// scrExecContext::Run(ctx, entry*, u32 argc, u64* args, u64* ret, u32 retc)
// [observed] 0x143de1f10: rcx/rdx/r8d/r9 saved, 5th arg read at [rsp+0x90] and 6th at [rsp+0x98]
// after a 0x68-byte frame, i.e. entry rsp+0x28 / +0x30. No 7th argument is read.
inline constexpr unsigned kRunArgCount = 6;

// [observed] vtable 0x14654fd20 slot 17 (+0x88) == 0x143de1f10 in the file.
inline constexpr unsigned kExecVtableRunSlot = 17;

// TLS publish site inside Run [observed, 0x143de2035..0x143de204f]:
//   +0x125  mov rax, gs:[0x58]          (65 48 8B 04 25 58 00 00 00; the host rewrites it to call <thunk>; nop4)
//   +0x12e  mov r14, [rax]              (4C 8B 30)
//   +0x131  mov r12, [r14+0x1478]       (4D 8B A6 78 14 00 00)
//   +0x138  mov [r14+0x1478], rsi       (49 89 B6 78 14 00 00)
inline constexpr std::uint32_t kRunTlsSiteOff = 0x125;
inline constexpr std::uint8_t kRunTlsSiteGs[9] = { 0x65, 0x48, 0x8B, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00 };
inline constexpr std::uint8_t kRunTlsSiteTail[17] = { 0x4C, 0x8B, 0x30, 0x4D, 0x8B, 0xA6, 0x78, 0x14, 0x00, 0x00,
                                                      0x49, 0x89, 0xB6, 0x78, 0x14, 0x00, 0x00 };

// [finding 13 §3 / types/parts/script-vm.json]
inline constexpr std::uint32_t kTlsActiveExecCtx = 0x1478;   // TLS block: active scrExecContext*
inline constexpr std::uint32_t kTlsBlockSize = 0x1480;       // [observed] .tls template size (manifest/REA)
inline constexpr std::uint32_t kCtxThread = 0x08;            // scrExecContext: scrThread*
inline constexpr std::uint32_t kThreadNameHash = 0x2b8;      // scrThread: script name hash (u32)
inline constexpr std::uint32_t kThreadState = 0x344;         // scrThread: 1 = running
inline constexpr std::uint32_t kThreadId = 0x348;            // scrThread: thread id

// Handler used to cross-check the native table against live code (manifest + anchor
// script.native.handler.set_entity_coords, finding 13 §4).
inline constexpr std::uint64_t kCheckNativeHash = 0x06843DA7060A026Bull;  // SET_ENTITY_COORDS

// Single-player guard: no build-dependent value. 1.0.2.1 has no session-state natives
// (findings/phase5/26); the guard is attestation + a TCP check of this process (guard.cpp).

// ---------------------------------------------------------------- game clock
// The runtime resolves the clock block from the manifest handler RVAs of these three natives by
// decoding their bodies (game_clock.cpp). Plugins use the patterns.hpp chain anchor world.game_timer
// (same GET_GAME_TIMER path; the self-test checks both agree). [observed 1.0.2.1]:
//   GET_GAME_TIMER  0x143e85810: ... js; call 0x1405f2ef0; cdqe; mov [ret],rax   core: 8B 05 rel32 (mov eax,[0x14abbef40]); C3
//   GET_FRAME_COUNT 0x143e85700: ... js; call 0x143eb1ef0                       core: 8B 05 rel32 (mov eax,[0x14abbef44]); C3
//   GET_FRAME_TIME  0x143e85740: ... js; call 0x140738420                       core: C5 FA 10 05 rel32 (vmovss xmm0,[0x14abbef4c]); C3
// The block is in .data (0xC0000040). Its only rip-relative writers are the init 0x142226130
// (zeroes +0, +8, writes +0xc/+0x10) and the updater 0x1422260b0..0x142226125, which adds the
// step in ms to +0 and a constant to +4 with one vpaddd, and keeps the previous +0 at +8.
// GET_GAME_TIMER sign-extends the u32 (cdqe), so +0 is a 32-bit millisecond counter that wraps.
inline constexpr std::uint64_t kNativeGetGameTimer = 0x4F67E8ECA7D3F667ull;
inline constexpr std::uint64_t kNativeGetFrameCount = 0x77DFA958FCF100C1ull;
inline constexpr std::uint64_t kNativeGetFrameTime = 0x5E72022914CE3C38ull;
inline constexpr std::uint32_t kClockFrameCountOff = 0x4;   // cross-check only
inline constexpr std::uint32_t kClockTimeStepOff = 0xc;     // cross-check only
inline constexpr std::uint32_t kClockBlockLen = 0x10;

// ---------------------------------------------------------------- file devices (mods/ overlay)
// fiDeviceRelative [observed 1.0.2.1]:
//   ctor 0x14165fa50: base ctor 0x14165fa80 (writes base vtable), then vtable = 0x1462fc810, [+8] = 0.
//     Located at run time as (site of the fs.device.relative.vtable anchor) - 0xd, after checking its bytes.
//   Init 0x1428cb210 (this, const char* root, bool readOnly (r8b), fiDevice* parent (r9)):
//     FullPath(root) into a 0x100 buffer; [+0x114] = readOnly; parent==null -> GetDevice(fullpath, readOnly);
//     [+8] = parent; safe-copy fullpath to [+0x10] (0x100 bytes); strips one trailing '/'.
//   MountAs 0x141fb2210 (this, const char* mountPoint) -> bool:
//     MountGlobal(mountPoint, this, [+0x114]); on success [+0x110] = strlen(mountPoint) - 1.
//   path translation 0x140643c10: if strlen(path) > [+0x110]: root + (path + [+0x110]), then parent->slot.
//   deleting dtor (vtable slot 0) 0x1457f6c00: operator delete(this, 0x118) -> sizeof == 0x118.
inline constexpr std::uint32_t kRelDevParent = 0x08;
inline constexpr std::uint32_t kRelDevRoot = 0x10;
inline constexpr std::uint32_t kRelDevRootCap = 0x100;
inline constexpr std::uint32_t kRelDevMountLen = 0x110;
inline constexpr std::uint32_t kRelDevReadOnly = 0x114;
inline constexpr std::uint32_t kRelDevSize = 0x118;         // also re-read from the dtor at run time
inline constexpr std::uint32_t kRelCtorSiteDelta = 0xd;     // vtable lea site - ctor start
inline constexpr std::uint8_t kRelCtorHead[9] = { 0x56, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x89, 0xCE, 0xE8 };
// fiDevice vtable slot used by the layered lookup (0x1428c8f8c call [rax+0x168]); -1 = absent.
inline constexpr std::uint32_t kDevVtGetAttributes = 0x168;

// Mount table [observed: MountGlobal 0x1412c5670, lookup 0x1428c8ec0, Unmount 0x1428cb450; live finding 30]:
//   anchor fs.mount.table (0x1478f5ec8) holds the entry array pointer; count at -4, capacity at -8.
//   entry (0x20 bytes): +0 char* prefix, +8 u16 readOnly flag, +0xa u16 prefix length,
//                       +0x10 i32 device capacity, +0x14 i32 device count, +0x18 fiDevice** devices.
//   lookup: longest prefix (strnicmp over +0xa chars); several devices -> walks devices[count-1 .. 0]
//   calling slot 0x168 until one returns != -1 (the last one is used regardless). All under the
//   fs.mount.lock critical section; MountGlobal takes the same lock and appends at devices[count].
//   Unmount(mountPoint) removes the whole entry and frees the device array, not the devices.
inline constexpr std::int32_t kMountCountOff = -4;
inline constexpr std::int32_t kMountCapOff = -8;
inline constexpr std::uint32_t kMountEntrySize = 0x20;
inline constexpr std::uint32_t kMountEntPrefix = 0x00;
inline constexpr std::uint32_t kMountEntFlag = 0x08;
inline constexpr std::uint32_t kMountEntDevCount = 0x14;
inline constexpr std::uint32_t kMountEntDevices = 0x18;
// MountGlobal refuses a mount point longer than this or one that does not end in '/' or '\'.
inline constexpr std::uint32_t kMountPointMax = 0xfd;

} // namespace g6rt::build
