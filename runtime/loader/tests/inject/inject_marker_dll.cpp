// Test payload for g6inject: on attach, writes marker_<pid>.txt next to itself containing
// "pid=<pid>". Only kernel32 file calls run under the loader lock.
#include <windows.h>

#include <cstdio>
#include <cwchar>

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);
    wchar_t path[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(inst, path, MAX_PATH * 2);
    while (n > 0 && path[n - 1] != L'\\') --n;
    if (n == 0 || n + 32 > MAX_PATH * 2) return TRUE;
    const DWORD pid = GetCurrentProcessId();
    std::swprintf(path + n, MAX_PATH * 2 - n, L"marker_%lu.txt", static_cast<unsigned long>(pid));
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return TRUE;
    char text[32];
    const int len = std::snprintf(text, sizeof text, "pid=%lu", static_cast<unsigned long>(pid));
    DWORD written = 0;
    if (len > 0) WriteFile(f, text, static_cast<DWORD>(len), &written, nullptr);
    CloseHandle(f);
    return TRUE;
}
