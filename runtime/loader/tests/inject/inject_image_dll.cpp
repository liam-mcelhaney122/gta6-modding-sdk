// Empty DLL that the injector test copies in as victory-image-0000000000000000.dll (the name the
// DexNom host gives its patched title image). It only has to appear in the module list.
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
