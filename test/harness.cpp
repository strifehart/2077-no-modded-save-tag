// Offline check: maps a game exe as an image and runs version detection, set selection and
// pattern validation from the plugin source without writing any patches.
#include "../src/NoModdedSaveTag.cpp"

int wmain(int argc, wchar_t** argv)
{
	g_log = stdout;
	if (argc < 2)
	{
		// Routing only, with synthetic versions.
		const char* name = nullptr;
		GameVersion v;
		v.major = 2; v.minor = 6; v.build = 0;
		Log("-- synthetic 2.6.0: %s", SelectPatchSet(v, &name) ? name : "none");
		v.minor = 2; v.build = 1;
		Log("-- synthetic 2.2.1: %s", SelectPatchSet(v, &name) ? name : "none");
		return 0;
	}
	HMODULE exe = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
	if (!exe)
	{
		Log("load failed: %lu", GetLastError());
		return 1;
	}
	GameVersion version;
	if (!ReadGameVersion(exe, version))
	{
		Log("version read failed");
		return 1;
	}
	Log("exe file version %u.%u.%u.%u, product version %u.%u.%u", version.fileMajor, version.fileMinor,
		version.fileBuild, version.filePrivate, version.major, version.minor, version.build);
	const char* name = nullptr;
	auto patches = SelectPatchSet(version, &name);
	if (!patches)
		return 1;
	uint8_t* text = nullptr;
	size_t size = 0;
	FindTextSection(reinterpret_cast<uint8_t*>(exe), &text, &size);
	for (auto& p : *patches)
	{
		uint8_t* match = nullptr;
		size_t hits = FindPattern(text, size, ParsePattern(p.pattern), &match);
		Log("  %-36s hits=%zu byte=0x%02X expected=0x%02X", p.name, hits, hits ? match[p.offset] : 0, p.expected);
	}
	return 0;
}
