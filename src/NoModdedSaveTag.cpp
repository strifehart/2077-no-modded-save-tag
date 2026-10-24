// NoModdedSaveTag — Ultimate ASI Loader plugin for Cyberpunk 2077 2.31 and 2.5 by strifehart.
// Awe Chooms. This is a simple ASI utility that aims to patch the base behaviour of the 2.5 update 
// that flags modded saves as "modded". This doesn't really have an impact on the pc side of things,
// but if you cross save between platforms it will stop your game on console from being able to unlock 
// achievements ~ supposedly. 
// This utility aims to remove that, cause flagging saves is not a very punk thing to do. 
// A save is tagged in two places:
//   - metadata.9.json "isModded" (saveMetadata offset 0x3AF), built from: engine mod lists
//     non-empty, OR (2.5 only) the ModdingSystem flag, OR (2.5 only) enabled mod.io mods.
//   - the ModdingSystem node in sav.dat: a sticky byte (system +0x48) that its save routine
//     ORs with "mod lists non-empty" (and on 2.5 "mod.io mods enabled") before writing it.
// We force every "mov al, 1" to 0 and turn every OR into AND, so both are always written as 0.
// The running game version is read from the exe's version resource and selects the patch set:
//		2.31 -> 2.31 set, 2.5 -> 2.5 set, newer than 2.5 -> 2.5 set with a warning, anything else ->
//		nothing. A set is applied only if every one of its patterns matches exactly once.
// Find me on nexus here : https://www.nexusmods.com/profile/strifehart
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

// Everything lives in an anonymous namespace so nothing leaks out of the DLL except DllMain.
namespace
{
// ================================================================================
// Settings
// ================================================================================

constexpr const char* kPluginVersion = "1.5";
constexpr const wchar_t* kGameExeName = L"Cyberpunk2077.exe"; // the only process we patch
constexpr size_t kMaxLogRuns = 20;       // launches kept in the log, including the current one
constexpr long kMaxLogSize = 512 * 1024; // safety net: a log bigger than this is not trimmed but started over

// ================================================================================
// Patch tables
// ================================================================================
//
// Each patch changes exactly one byte of the game's machine code. Two kinds of change are used:
//
//   B0 01  mov al, 1   ->  B0 00  mov al, 0
//       The game computes "are mods loaded?" into AL. Forcing the 1 to a 0 makes the
//       answer always "no", whichever branch the code takes.
//
//   08 xx  or  [flag], reg   ->  20 xx  and [flag], reg
//       The game merges further "modded" sources into the flag with OR. Swapping the opcode
//       to AND keeps the same instruction length and operands, but since the flag is already
//       0 at that point, 0 AND anything stays 0.
//
// Because every replacement is the same size as the original byte, no instruction moves and
// no jump target changes; the game's code is otherwise untouched.
//
// Patterns are copied from the game exe with "??" over bytes that differ between builds
// (RIP-relative addresses of globals). Each pattern was checked to match exactly once in its
// game version. Where two patches share a pattern, they change different bytes of one match.

struct Patch
{
	const char* name;
	const char* pattern;   // hex bytes, "??" = wildcard
	size_t offset;         // byte within the match to overwrite
	uint8_t expected;      // original byte at that offset
	uint8_t replacement;
	uint8_t* address = nullptr; // filled in once the pattern is found
};

// 2.5: the metadata builder (exe+0x22FF7E0 in the GOG build) ORs three sources into isModded,
// and the ModdingSystem save routine (exe+0x2A1EA64) ORs two sources into its sticky byte.
std::vector<Patch> g_patches25 = {
	{ "metadata: mod lists -> mov al,0",   "83 7B 0C 00 77 0A 83 78 0C 00 77 04 32 C0 EB 02 B0 01 48 8B 15 ?? ?? ?? ?? 48 8B CF 88 86 AF 03 00 00", 17, 0x01, 0x00 },
	{ "metadata: system flag or -> and",   "4C 8D B6 B0 03 00 00 0F B6 48 48 08 8E AF 03 00 00", 11, 0x08, 0x20 },
	{ "metadata: mod.io or -> and",        "FF D3 08 86 AF 03 00 00 48 8B 44 24", 2, 0x08, 0x20 },
	{ "ModdingSystem: mod lists -> 0",     "83 7B 0C 00 77 0A 83 78 0C 00 77 04 32 C0 EB 02 B0 01 48 8B 0D ?? ?? ?? ?? 48 8D 5F 48 08 03", 17, 0x01, 0x00 },
	{ "ModdingSystem: mod lists or -> and", "83 7B 0C 00 77 0A 83 78 0C 00 77 04 32 C0 EB 02 B0 01 48 8B 0D ?? ?? ?? ?? 48 8D 5F 48 08 03", 29, 0x08, 0x20 },
	{ "ModdingSystem: mod.io or -> and",   "FF 90 80 01 00 00 08 03 48 8D 4C 24", 6, 0x08, 0x20 },
};

// 2.31: the metadata builder (exe+0xE61CB0) only checks the mod lists, and the ModdingSystem
// save routine (exe+0x24764D8) ORs the mod lists into its sticky byte. No mod.io in 2.31.
std::vector<Patch> g_patches231 = {
	{ "metadata: mod lists -> mov al,0",   "44 39 73 0C 77 09 44 39 70 0C 41 8A C6 76 02 B0 01 48 8D 4F 28 88 87 AF 03 00 00", 16, 0x01, 0x00 },
	{ "ModdingSystem: mod lists -> 0",     "83 7B 0C 00 77 0A 83 78 0C 00 77 04 32 C0 EB 02 B0 01 08 46 48", 17, 0x01, 0x00 },
	{ "ModdingSystem: or -> and",          "83 7B 0C 00 77 0A 83 78 0C 00 77 04 32 C0 EB 02 B0 01 08 46 48", 18, 0x08, 0x20 },
};

// ================================================================================
// Game version
// ================================================================================

// Game version as stored in the exe's fixed product version: 2.31 = 2.3.1, 2.5 = 2.5.0.
// The file version (e.g. 3.0.88.46235) is only logged, to help identify exact builds.
struct GameVersion
{
	WORD major = 0, minor = 0, build = 0;
	WORD fileMajor = 0, fileMinor = 0, fileBuild = 0, filePrivate = 0;

	unsigned Key() const { return (major << 16) | (minor << 8) | build; }
};

// Packs major.minor.build into one number so versions can be compared with < and >.
constexpr unsigned VersionKey(WORD major, WORD minor, WORD build) { return (major << 16) | (minor << 8) | build; }
constexpr unsigned kVersion231 = VersionKey(2, 3, 1);
constexpr unsigned kVersion25 = VersionKey(2, 5, 0);

// ================================================================================
// Logging
// ================================================================================

FILE* g_log = nullptr;

// printf-style line logger. Flushes after every line so the log is complete even if the
// game crashes later in the session. Does nothing if the log file could not be opened.
void Log(const char* fmt, ...)
{
	if (!g_log)
		return;
	va_list args;
	va_start(args, fmt);
	vfprintf(g_log, fmt, args);
	va_end(args);
	fputc('\n', g_log);
	fflush(g_log);
}

// ================================================================================
// Pattern scanning
// ================================================================================

// Turns a pattern string such as "B0 01 ?? 48" into bytes {0xB0, 0x01, -1, 0x48},
// where -1 marks a wildcard that matches any byte. Spaces are ignored.
std::vector<int> ParsePattern(const char* text)
{
	std::vector<int> bytes;
	for (const char* p = text; *p;)
	{
		if (*p == ' ')
		{
			++p;
			continue;
		}
		if (p[0] == '?')
		{
			bytes.push_back(-1);
			p += (p[1] == '?') ? 2 : 1;
			continue;
		}
		char hex[3] = { p[0], p[1], 0 };
		bytes.push_back(static_cast<int>(strtoul(hex, nullptr, 16)));
		p += 2;
	}
	return bytes;
}

// Slides the pattern over the memory block one byte at a time and counts every position
// where all non-wildcard bytes match. Returns the number of matches; stores the first in *out.
// The count matters: a pattern that matches more than once is ambiguous, and Apply() treats
// that as a failure rather than guessing which match is the right one.
size_t FindPattern(uint8_t* start, size_t size, const std::vector<int>& pat, uint8_t** out)
{
	size_t hits = 0;
	for (size_t i = 0; i + pat.size() <= size; ++i)
	{
		size_t j = 0;
		for (; j < pat.size(); ++j)
		{
			if (pat[j] >= 0 && start[i + j] != pat[j])
				break;
		}
		if (j == pat.size())
		{
			if (hits++ == 0)
				*out = start + i;
		}
	}
	return hits;
}

// ================================================================================
// Log file setup
// ================================================================================

// Returns the byte offset where each launch block starts in the log text. A block starts at a
// divider line (only '#' characters) that is directly followed by the "NoModdedSaveTag <ver> |"
// title line written by LogHeader().
std::vector<size_t> FindRunStarts(const std::string& text)
{
	std::vector<size_t> starts;
	for (size_t pos = 0; pos < text.size();)
	{
		size_t end = text.find('\n', pos);
		if (end == std::string::npos)
			end = text.size();
		size_t len = end - pos;
		if (len > 0 && text[pos + len - 1] == '\r')
			--len;

		size_t nonHash = text.find_first_not_of('#', pos);
		bool isDivider = len >= 10 && (nonHash == std::string::npos || nonHash >= pos + len);
		if (isDivider && end + 1 < text.size() && text.compare(end + 1, 16, "NoModdedSaveTag ") == 0)
			starts.push_back(pos);
		pos = end + 1;
	}
	return starts;
}

// Keeps only the newest (kMaxLogRuns - 1) launch blocks, so that with the launch about to be
// written the log holds kMaxLogRuns at most. Anything before the oldest kept block (older
// launches, or lines from plugin versions before 1.2 that had no header) is dropped.
// If the file can't be read or has no recognisable blocks, it is left alone.
// The trimmed text is written to a temporary file that then replaces the log in one step, so a
// failed write (disk full, file locked by antivirus) leaves the old log intact.
void TrimLog(const wchar_t* path)
{
	FILE* file = nullptr;
	if (_wfopen_s(&file, path, L"rb") != 0 || !file)
		return;
	std::string text;
	char buffer[4096];
	size_t read;
	while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0)
		text.append(buffer, read);
	fclose(file);

	std::vector<size_t> starts = FindRunStarts(text);
	const size_t keep = kMaxLogRuns - 1;
	if (starts.size() <= keep)
		return;

	// Write in binary mode so the existing CRLF line endings are copied unchanged.
	std::wstring tempPath = std::wstring(path) + L".tmp";
	if (_wfopen_s(&file, tempPath.c_str(), L"wb") != 0 || !file)
		return;
	size_t from = starts[starts.size() - keep];
	size_t length = text.size() - from;
	bool written = fwrite(text.data() + from, 1, length, file) == length;
	written = fclose(file) == 0 && written;
	if (!written || !MoveFileExW(tempPath.c_str(), path, MOVEFILE_REPLACE_EXISTING))
		DeleteFileW(tempPath.c_str());
}

// Opens the log next to the plugin: takes this DLL's own path (…\plugins\NoModdedSaveTag.asi)
// and swaps the extension for ".log". The log keeps the last kMaxLogRuns launches in a single
// file: older launch blocks are trimmed off the top before the new one is appended. A file
// larger than kMaxLogSize (which normal use never reaches) is simply started over.
// Uses the wide-character (Unicode) file APIs so install paths with non-Latin characters work.
void OpenLog(HMODULE self)
{
	wchar_t path[MAX_PATH];
	DWORD len = GetModuleFileNameW(self, path, MAX_PATH);
	if (len == 0 || len >= MAX_PATH - 4)
		return;
	wchar_t* dot = wcsrchr(path, L'.');
	if (dot)
		wcscpy_s(dot, MAX_PATH - (dot - path), L".log");

	const wchar_t* mode = L"a";
	WIN32_FILE_ATTRIBUTE_DATA info;
	if (GetFileAttributesExW(path, GetFileExInfoStandard, &info))
	{
		if (info.nFileSizeHigh != 0 || info.nFileSizeLow > kMaxLogSize)
			mode = L"w";
		else
			TrimLog(path);
	}
	_wfopen_s(&g_log, path, mode);
}

// Writes the per-launch banner: a divider, plugin version and local date/time, then the path
// of the game exe that loaded us (useful when several installs share one save folder).
void LogHeader()
{
	SYSTEMTIME now;
	GetLocalTime(&now);
	Log("");
	Log("################################################################################################");
	Log("NoModdedSaveTag %s | %04u-%02u-%02u %02u:%02u:%02u", kPluginVersion,
		now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
	Log("################################################################################################");

	// Convert to UTF-8 so paths with non-Latin characters are logged in full (printf's %ls
	// stops at the first character that the C locale can't represent).
	wchar_t exePath[MAX_PATH];
	char exePathUtf8[MAX_PATH * 4];
	if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) &&
		WideCharToMultiByte(CP_UTF8, 0, exePath, -1, exePathUtf8, sizeof(exePathUtf8), nullptr, nullptr))
		Log("game exe: %s", exePathUtf8);
}

// ================================================================================
// Version detection
// ================================================================================

// Reads VS_FIXEDFILEINFO straight from the loaded exe's resources (no version.dll calls,
// since version.dll is the ASI loader itself).
// Every Windows exe carries a VERSIONINFO resource (what Explorer shows under
// Properties > Details). Inside it is a fixed-layout VS_FIXEDFILEINFO block that starts with
// the signature 0xFEEF04BD; we find the resource, then scan it (4-byte aligned) for that
// signature and read the version numbers that follow.
bool ReadGameVersion(HMODULE exe, GameVersion& out)
{
	HRSRC res = FindResourceW(exe, MAKEINTRESOURCEW(VS_VERSION_INFO), MAKEINTRESOURCEW(16) /* RT_VERSION */);
	if (!res)
		return false;
	HGLOBAL handle = LoadResource(exe, res);
	auto data = handle ? static_cast<const uint8_t*>(LockResource(handle)) : nullptr;
	DWORD size = SizeofResource(exe, res);
	if (!data || size < sizeof(VS_FIXEDFILEINFO))
		return false;

	for (DWORD i = 0; i + sizeof(VS_FIXEDFILEINFO) <= size; i += 4)
	{
		auto info = reinterpret_cast<const VS_FIXEDFILEINFO*>(data + i);
		if (info->dwSignature != 0xFEEF04BD)
			continue;
		out.major = HIWORD(info->dwProductVersionMS);
		out.minor = LOWORD(info->dwProductVersionMS);
		out.build = HIWORD(info->dwProductVersionLS);
		out.fileMajor = HIWORD(info->dwFileVersionMS);
		out.fileMinor = LOWORD(info->dwFileVersionMS);
		out.fileBuild = HIWORD(info->dwFileVersionLS);
		out.filePrivate = LOWORD(info->dwFileVersionLS);
		return true;
	}
	return false;
}

// Picks the patch set for the detected version; returns nullptr if unsupported.
//   2.31          -> 2.31 patches
//   2.5           -> 2.5 patches
//   newer than 2.5 -> 2.5 patches, with a warning (a future patch may have changed the code;
//                    if so the pattern checks in Apply() fail safely and nothing is patched)
//   anything else -> nothing
std::vector<Patch>* SelectPatchSet(const GameVersion& v, const char** setName)
{
	unsigned key = v.Key();
	if (key == kVersion231)
	{
		*setName = "2.31";
		Log("detected game version 2.31 -> using 2.31 logic");
		return &g_patches231;
	}
	if (key == kVersion25)
	{
		*setName = "2.5";
		Log("detected game version 2.5 -> using 2.5 logic");
		return &g_patches25;
	}
	if (key > kVersion25)
	{
		*setName = "2.5";
		Log("WARNING: detected game version %u.%u.%u is newer than 2.5 -> trying 2.5 logic", v.major, v.minor, v.build);
		Log("WARNING: this version is untested; the patches may not match and nothing may be patched");
		return &g_patches25;
	}
	Log("ERROR: game version %u.%u.%u is not supported (supported: 2.31, 2.5); nothing patched", v.major, v.minor, v.build);
	return nullptr;
}

// ================================================================================
// Patching
// ================================================================================

// Finds the game's code in memory. A loaded exe starts with its PE headers: the DOS header
// points to the NT headers, which are followed by a table of sections. ".text" is the
// section holding the compiled code, so that is the only range we scan.
bool FindTextSection(uint8_t* base, uint8_t** text, size_t* textSize)
{
	auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
	auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
	auto section = IMAGE_FIRST_SECTION(nt);
	for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
	{
		if (memcmp(section->Name, ".text", 5) == 0)
		{
			*text = base + section->VirtualAddress;
			*textSize = section->Misc.VirtualSize;
			return true;
		}
	}
	return false;
}

// VirtualProtect, called through a pointer so the offline test harness can simulate a failure.
using VirtualProtectFn = BOOL(WINAPI*)(LPVOID, SIZE_T, DWORD, PDWORD);
VirtualProtectFn g_virtualProtect = VirtualProtect;

// Returns a patch's pattern as it looks after patching: the original bytes with the
// replacement byte of every patch in the set that shares this pattern.
std::vector<int> PatchedPattern(const std::vector<Patch>& patches, const Patch& p)
{
	std::vector<int> bytes = ParsePattern(p.pattern);
	for (const auto& q : patches)
	{
		if (strcmp(q.pattern, p.pattern) == 0 && q.offset < bytes.size())
			bytes[q.offset] = q.replacement;
	}
	return bytes;
}

enum class PatchState
{
	Unpatched,      // every pattern matches exactly once with its original byte: safe to patch
	AlreadyPatched, // every pattern matches exactly once in its patched form: nothing to do
	Failed,         // anything else (a different build, or partly patched): don't touch it
};

// Locates every patch of the set and works out which state the game's code is in. Each
// patch's address is filled in on success. "Already patched" happens when a second copy of
// this plugin is installed (e.g. one in bin\x64 and one in plugins): the first copy patches
// the code, and the second must not report that as an error.
PatchState LocatePatches(uint8_t* text, size_t textSize, std::vector<Patch>& patches, const char* setName)
{
	size_t unpatched = 0, patched = 0;
	for (auto& p : patches)
	{
		uint8_t* match = nullptr;
		size_t hits = FindPattern(text, textSize, ParsePattern(p.pattern), &match);
		if (hits == 1 && match[p.offset] == p.expected)
		{
			p.address = match + p.offset;
			++unpatched;
			continue;
		}
		size_t patchedHits = FindPattern(text, textSize, PatchedPattern(patches, p), &match);
		if (hits == 0 && patchedHits == 1 && match[p.offset] == p.replacement)
		{
			p.address = match + p.offset;
			++patched;
			continue;
		}
		Log("ERROR: %s pattern '%s' matched %zu times (expected 1), %zu times in patched form; nothing patched",
			setName, p.name, hits, patchedHits);
		return PatchState::Failed;
	}
	if (unpatched == patches.size())
		return PatchState::Unpatched;
	if (patched == patches.size())
		return PatchState::AlreadyPatched;
	Log("ERROR: %s code is partly patched (%zu of %zu patches already applied); nothing changed",
		setName, patched, patches.size());
	return PatchState::Failed;
}

// Writes one byte of code. Code memory is read-only + executable, so the byte is made writable
// just long enough to change it, then the original protection is restored. FlushInstructionCache
// tells the CPU the code changed so it doesn't run a stale copy. Returns false, without writing,
// if the memory can't be made writable; writing to it anyway would crash the game.
bool WriteCodeByte(uint8_t* address, uint8_t value)
{
	DWORD old;
	if (!g_virtualProtect(address, 1, PAGE_EXECUTE_READWRITE, &old))
		return false;
	*address = value;
	DWORD unused;
	if (!g_virtualProtect(address, 1, old, &unused))
		Log("WARNING: could not restore memory protection after writing (harmless)");
	FlushInstructionCache(GetCurrentProcess(), address, 1);
	return true;
}

// Applies every patch in order. If one can't be written, the patches already written are put
// back to their original bytes, so the set stays all-or-nothing. Returns true if all applied.
bool ApplyPatches(uint8_t* base, std::vector<Patch>& patches)
{
	for (size_t i = 0; i < patches.size(); ++i)
	{
		Patch& p = patches[i];
		if (!WriteCodeByte(p.address, p.replacement))
		{
			DWORD error = GetLastError();
			Log("ERROR: could not make the code writable for '%s' (Windows error %lu); undoing %zu earlier patch(es)",
				p.name, error, i);
			for (size_t j = i; j-- > 0;)
			{
				if (WriteCodeByte(patches[j].address, patches[j].expected))
					Log("undid '%s'", patches[j].name);
				else
					Log("ERROR: could not undo '%s'", patches[j].name);
			}
			return false;
		}
		Log("patched '%s' at exe+0x%llX", p.name, static_cast<unsigned long long>(p.address - base));
	}
	return true;
}

// Some ASI loaders (e.g. Ultimate ASI Loader v10) also load plugins into other programs in
// bin\x64 that import version.dll, such as REDEngineErrorReporter.exe, the crash reporter
// the game starts alongside itself. We only belong in the game, so check the file name of
// the exe that started this process.
bool IsGameProcess()
{
	wchar_t exePath[MAX_PATH];
	DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
	if (len == 0 || len >= MAX_PATH)
		return false;
	const wchar_t* name = wcsrchr(exePath, L'\\');
	name = name ? name + 1 : exePath;
	return _wcsicmp(name, kGameExeName) == 0;
}

// The whole plugin in order: host check -> log header -> detect version -> pick patch set ->
// find and validate every patch -> apply them all. Any failure before the last step returns
// early with an explanation in the log, leaving the game completely unmodified. A set is never
// half-applied.
void Apply(HMODULE self)
{
	// Not the game (e.g. the crash reporter): leave silently, without even touching the log.
	if (!IsGameProcess())
		return;

	OpenLog(self);
	LogHeader();

	// GetModuleHandle(nullptr) is the exe that started the process, i.e. Cyberpunk2077.exe.
	// Its handle is also its base address in memory.
	auto exe = GetModuleHandleW(nullptr);
	auto base = reinterpret_cast<uint8_t*>(exe);

	// 1. Which game version is this?
	GameVersion version;
	if (!ReadGameVersion(exe, version))
	{
		Log("ERROR: could not read the game's version info; nothing patched");
		return;
	}
	Log("exe file version %u.%u.%u.%u, product version %u.%u.%u", version.fileMajor, version.fileMinor,
		version.fileBuild, version.filePrivate, version.major, version.minor, version.build);

	// 2. Which patches belong to it?
	const char* setName = nullptr;
	std::vector<Patch>* patches = SelectPatchSet(version, &setName);
	if (!patches)
		return;

	// 3. Where is the game's code?
	uint8_t* text = nullptr;
	size_t textSize = 0;
	if (!FindTextSection(base, &text, &textSize))
	{
		Log("ERROR: .text section not found; nothing patched");
		return;
	}

	// 4. Locate and validate every patch before touching anything. Each pattern must match
	//    exactly once with the byte we are about to change still holding its original value;
	//    otherwise this isn't the code we expect (different build). If instead every patch is
	//    found already applied, another copy of this plugin got there first.
	switch (LocatePatches(text, textSize, *patches, setName))
	{
	case PatchState::Failed:
		return;
	case PatchState::AlreadyPatched:
		Log("OK (%s logic): the code is already patched (another copy of this plugin loaded first?); nothing to do", setName);
		return;
	case PatchState::Unpatched:
		break;
	}

	// 5. Apply, all or nothing.
	if (!ApplyPatches(base, *patches))
	{
		Log("ERROR: nothing patched; the game runs unmodified");
		return;
	}
	Log("OK (%s logic): new saves will be written with isModded = false and ModdingSystem flag = 0", setName);
}

// Runs Apply() and turns any C++ exception (e.g. running out of memory) into a log line.
void ApplyCatchingExceptions(HMODULE self)
{
	try
	{
		Apply(self);
	}
	catch (const std::exception& e)
	{
		Log("ERROR: unexpected failure (%s); patching stopped, the game continues", e.what());
	}
	catch (...)
	{
		Log("ERROR: unexpected failure; patching stopped, the game continues");
	}
}

// Runs fn(self) behind a structured exception handler, so that even a hardware-level fault
// inside the plugin (such as an access violation) is logged instead of crashing the game at
// launch. MSVC doesn't allow __try in a function that has C++ objects to clean up, which is why
// this is a separate function that only calls through a pointer.
void RunGuarded(void (*fn)(HMODULE), HMODULE self)
{
	__try
	{
		fn(self);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Log("ERROR: unexpected failure (exception 0x%08lX); patching stopped, the game continues", GetExceptionCode());
	}
}
} // namespace

// ================================================================================
// Entry point
// ================================================================================

// Windows calls DllMain when the ASI loader (version.dll) loads this plugin, which happens
// while the game is still starting up, before any of its own code has run, so the patches
// are in place long before the first save. We only care about the initial attach: thread
// notifications are switched off, the patches are applied once (behind the crash guards
// above), and the log is closed. DllMain always returns TRUE: whatever happens in the plugin,
// the game keeps loading.
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(module);
		RunGuarded(ApplyCatchingExceptions, module);
		if (g_log)
		{
			fclose(g_log);
			g_log = nullptr;
		}
	}
	return TRUE;
}
