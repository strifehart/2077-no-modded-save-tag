// Offline tests for the 1.5 hardening: already-patched detection, VirtualProtect failure with
// rollback, partial patch detection, and the crash guard. Patches a game exe mapped into this
// process only; the file on disk is never touched.
#include "../src/NoModdedSaveTag.cpp"

namespace
{
int g_failOnCall = -1; // which "make writable" call should fail (0-based), -1 = never
int g_calls = 0;

BOOL WINAPI FlakyVirtualProtect(LPVOID address, SIZE_T size, DWORD protect, PDWORD old)
{
	if (protect == PAGE_EXECUTE_READWRITE && g_calls++ == g_failOnCall)
	{
		SetLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	return VirtualProtect(address, size, protect, old);
}

const char* StateName(PatchState s)
{
	return s == PatchState::Unpatched ? "Unpatched" : s == PatchState::AlreadyPatched ? "AlreadyPatched" : "Failed";
}

int g_failures = 0;
void Check(bool ok, const char* what)
{
	printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		++g_failures;
}

bool AllBytes(const std::vector<Patch>& patches, bool patched)
{
	for (const auto& p : patches)
	{
		if (*p.address != (patched ? p.replacement : p.expected))
			return false;
	}
	return true;
}

void Crash(HMODULE) { *static_cast<volatile int*>(nullptr) = 1; }
void Throw(HMODULE) { throw std::bad_alloc(); }
}

int wmain(int argc, wchar_t** argv)
{
	g_log = stdout;
	if (argc < 2)
		return 1;

	for (int round = 0; round < 3; ++round)
	{
		HMODULE exe = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
		auto base = reinterpret_cast<uint8_t*>(exe);
		uint8_t* text = nullptr;
		size_t size = 0;
		FindTextSection(base, &text, &size);
		GameVersion version; ReadGameVersion(exe, version); const char* setName = nullptr; std::vector<Patch>& set = *SelectPatchSet(version, &setName); std::vector<Patch> patches = set;

		if (round == 0)
		{
			printf("== normal apply, then a second copy of the plugin\n");
			Check(LocatePatches(text, size, patches, setName) == PatchState::Unpatched, "fresh code is Unpatched");
			Check(ApplyPatches(base, patches), "ApplyPatches succeeds");
			Check(AllBytes(patches, true), "all bytes now hold their patched values");
			std::vector<Patch> second = set;
			PatchState s = LocatePatches(text, size, second, setName);
			printf("  second copy sees: %s\n", StateName(s));
			Check(s == PatchState::AlreadyPatched, "second copy detects AlreadyPatched");
		}
		else if (round == 1)
		{
			printf("== VirtualProtect fails on the last patch\n");
			Check(LocatePatches(text, size, patches, setName) == PatchState::Unpatched, "fresh code is Unpatched");
			g_virtualProtect = FlakyVirtualProtect;
			g_calls = 0;
			g_failOnCall = static_cast<int>(patches.size()) - 1;
			Check(!ApplyPatches(base, patches), "ApplyPatches reports failure");
			g_virtualProtect = VirtualProtect;
			Check(AllBytes(patches, false), "every byte rolled back to its original value");
		}
		else
		{
			printf("== partly patched code\n");
			Check(LocatePatches(text, size, patches, setName) == PatchState::Unpatched, "fresh code is Unpatched");
			WriteCodeByte(patches[0].address, patches[0].replacement);
			std::vector<Patch> again = set;
			Check(LocatePatches(text, size, again, setName) == PatchState::Failed, "partly patched code is refused");
		}
		FreeLibrary(exe);
	}

	printf("== crash guard\n");
	RunGuarded(Crash, nullptr);
	Check(true, "access violation caught, process still running");
	RunGuarded(Throw, nullptr);
	Check(true, "C++ exception caught, process still running");

	printf("%s (%d failure(s))\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
	return g_failures;
}
