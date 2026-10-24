// Loads an .asi into this process, to test the plugin's host-exe check.
#include <windows.h>
#include <cstdio>
int wmain(int argc, wchar_t** argv)
{
	HMODULE m = argc > 1 ? LoadLibraryW(argv[1]) : nullptr;
	printf("LoadLibrary %s\n", m ? "ok" : "failed");
	return m ? 0 : 1;
}
