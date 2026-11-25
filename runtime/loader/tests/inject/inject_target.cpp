// Stand-in for the DexNom title process in the injector test: optionally maps a DLL named like the
// host's patched image (victory-image-<16 hex>.dll) from its own folder, then sleeps.
//   g6inject_target.exe [--image <file name>] [--exit-after-ms N]      (default lifetime 120 s)
#include <windows.h>

#include <cwchar>
#include <string>

int wmain(int argc, wchar_t** argv) {
    const wchar_t* image = nullptr;
    DWORD life_ms = 120000;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::wcscmp(argv[i], L"--image") == 0) image = argv[i + 1];
        else if (std::wcscmp(argv[i], L"--exit-after-ms") == 0) life_ms = static_cast<DWORD>(std::wcstoul(argv[i + 1], nullptr, 10));
        else return 2;
    }
    if (image) {
        wchar_t self[MAX_PATH * 2];
        DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
        while (n > 0 && self[n - 1] != L'\\') --n;
        const std::wstring path = std::wstring(self, n) + image;
        if (!LoadLibraryW(path.c_str())) return 4;
    }
    Sleep(life_ms);
    return 0;
}
