// Radahn League core, hot-reloaded by the loader (rl_start / rl_stop).
// Finds the game's managers (FD4 singleton lookup from The Grand Archives' Elden Ring table, plus their
// GameDataMan byte pattern) and logs the player character and Torrent's ride state.
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

static std::wstring g_dir;
static std::atomic<bool> g_running{ false };
static HANDLE g_thread;

static FILE* g_log;
static CRITICAL_SECTION g_logLock;
static bool g_logLockInit;

static void Log(const char* fmt, ...)
{
    if (!g_logLockInit) { InitializeCriticalSection(&g_logLock); g_logLockInit = true; }
    EnterCriticalSection(&g_logLock);
    if (!g_log) g_log = _wfsopen((g_dir + L"rlmod.log").c_str(), L"a", 0x40 /* _SH_DENYNO */);
    FILE* f = g_log;
    if (!f) { LeaveCriticalSection(&g_logLock); return; }
    time_t now = time(nullptr);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%H:%M:%S", localtime(&now));
    fprintf(f, "[%s] ", stamp);
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputc('\n', f);
    fflush(f);
    LeaveCriticalSection(&g_logLock);
}

struct Region { uintptr_t start = 0, end = 0; bool Has(uintptr_t p) const { return p >= start && p < end; } };

static Region Section(const char* name)
{
    auto base = (uintptr_t)GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
        if (strncmp((const char*)sec->Name, name, 8) == 0)
            return { base + sec->VirtualAddress, base + sec->VirtualAddress + sec->Misc.VirtualSize };
    return {};
}

static std::vector<uintptr_t> Scan(const Region& r, const char* pattern)
{
    std::vector<int> pat; // -1 = wildcard
    for (const char* p = pattern; *p;)
    {
        if (*p == ' ') { p++; continue; }
        if (*p == '?') { pat.push_back(-1); while (*p == '?') p++; continue; }
        pat.push_back((int)strtoul(std::string(p, 2).c_str(), nullptr, 16)); p += 2;
    }
    std::vector<uintptr_t> hits;
    for (uintptr_t p = r.start; p + pat.size() <= r.end; p++)
    {
        size_t i = 0;
        for (; i < pat.size(); i++) if (pat[i] >= 0 && *(uint8_t*)(p + i) != pat[i]) break;
        if (i == pat.size()) hits.push_back(p);
    }
    return hits;
}

static int32_t Rel(uintptr_t at) { return *(int32_t*)at; }

// name -> address of the static pointer holding the singleton instance
static std::unordered_map<std::string, uintptr_t> FindSingletons()
{
    Region text = Section(".text"), data = Section(".data"), rdata = Section(".rdata");
    std::unordered_map<std::string, uintptr_t> out;
    for (uintptr_t c : Scan(text,
        "48 8b ? ? ? ? ? 48 85 ? 75 2e 48 8d 0d ? ? ? ? e8 ? ? ? ? 4c 8b c8 4c 8d 05 ? ? ? ? ba ? ? 00 00 48 8d 0d ? ? ? ? e8 ? ? ? ?"))
    {
        uintptr_t staticAddr = c + 7 + Rel(c + 3);
        uintptr_t runtimeClass = c + 19 + Rel(c + 15);
        uintptr_t getName = c + 24 + Rel(c + 20);
        auto path = (const char*)(c + 46 + Rel(c + 42));
        if (!data.Has(staticAddr) || !data.Has(runtimeClass) || !text.Has(getName) || !rdata.Has((uintptr_t)path)) continue;
        size_t len = strnlen(path, 256);
        if (len < 14 || len == 256 || strcmp(path + len - 14, "FD4Singleton.h") != 0) continue;
        auto name = ((const char* (*)(uintptr_t))getName)(runtimeClass);
        if (!name) continue;
        const char* simple = strrchr(name, ':');
        out.emplace(simple ? simple + 1 : name, staticAddr);
    }
    return out;
}

// First match of a "mov reg, [rip+rel32]" pattern -> the static address it reads
static uintptr_t StaticFromPattern(const char* pattern)
{
    auto hits = Scan(Section(".text"), pattern);
    return hits.empty() ? 0 : hits[0] + 7 + Rel(hits[0] + 3);
}

template <class T> static bool Read(uintptr_t at, T& out)
{
    SIZE_T got = 0;
    return at && ReadProcessMemory(GetCurrentProcess(), (void*)at, &out, sizeof(T), &got) && got == sizeof(T);
}

// Cheat Engine style [[[base]+o1]+o2]...: dereference, add offset, repeat. Returns 0 on any bad pointer.
static uintptr_t Chase(uintptr_t base, std::initializer_list<uintptr_t> offsets)
{
    uintptr_t p = base;
    for (auto o : offsets)
    {
        uintptr_t next;
        if (!Read(p, next) || !next) return 0;
        p = next + o;
    }
    return p;
}

// --- Hiding the rider ---
// PlayerIns+0x1C5 bit 3 is the character's "draw" flag (found via an older community table's "ToggleDraw",
// confirmed on 2.7.1.0): clearing it hides the player model; the game does not set it back by itself.
constexpr uintptr_t kDrawFlagOffset = 0x1C5;
constexpr uint8_t kDrawFlagBit = 1 << 3;

static void SetHidden(uintptr_t player, bool hidden)
{
    uint8_t v = 0;
    if (!player || !Read(player + kDrawFlagOffset, v)) return;
    uint8_t nv = hidden ? (v & ~kDrawFlagBit) : (v | kDrawFlagBit);
    if (nv == v) return;
    *(volatile uint8_t*)(player + kDrawFlagOffset) = nv;
    Log(hidden ? "hid player" : "showed player");
}

// --- Camera: pull the chase camera back and widen the view ---
// Game params live in memory under CSRegulationManager. Layout (community-documented):
//   manager +0x18/+0x20 = begin/end of ParamResCap*; ParamResCap +0x18 = wide-string name (DLWString),
//   +0x80 = header, header +0x80 = table; table +0x0A = row count (u16), +0x40 = rows of {id, dataOffset, end} (24 bytes).
// LockCamParam rows: +0x00 camDistTarget, +0x14 camFovY (degrees).
static uintptr_t g_regulationManager;

static uintptr_t FindParamTable(const wchar_t* wanted)
{
    uintptr_t rm = 0, it = 0, end = 0;
    if (!Read(g_regulationManager, rm) || !rm || !Read(rm + 0x18, it) || !Read(rm + 0x20, end)) return 0;
    for (; it && it < end; it += 8)
    {
        uintptr_t cap = 0; if (!Read(it, cap) || !cap) continue;
        uint64_t len = 0, capacity = 0; Read(cap + 0x18 + 0x10, len); Read(cap + 0x18 + 0x18, capacity);
        uintptr_t chars = cap + 0x18; if (capacity > 7) Read(cap + 0x18, chars);
        wchar_t name[64] = {}; if (len >= 64) continue;
        SIZE_T got = 0; ReadProcessMemory(GetCurrentProcess(), (void*)chars, name, len * sizeof(wchar_t), &got);
        if (wcscmp(name, wanted) != 0) continue;
        uintptr_t header = 0, table = 0;
        if (Read(cap + 0x80, header) && header && Read(header + 0x80, table)) return table;
    }
    return 0;
}

struct CamRow { uintptr_t row; float dist, fov; };
static std::vector<CamRow> g_camOriginal;

static bool ApplyCamera(float distMult, float fov)
{
    if (g_camOriginal.empty())
    {
        uintptr_t t = FindParamTable(L"LockCamParam");
        uint16_t rows = 0; if (!t || !Read(t + 0x0A, rows) || !rows) return false; // params not loaded yet (early in startup): retry later
        for (int i = 0; i < rows; i++)
        {
            uint64_t off = 0; if (!Read(t + 0x40 + i * 24 + 8, off)) continue;
            CamRow r{ t + off }; Read(r.row + 0x00, r.dist); Read(r.row + 0x14, r.fov);
            g_camOriginal.push_back(r);
        }
        Log("LockCamParam: %zu rows (first: dist %.2f fov %.1f)", g_camOriginal.size(), g_camOriginal.empty() ? 0.f : g_camOriginal[0].dist, g_camOriginal.empty() ? 0.f : g_camOriginal[0].fov);
    }
    for (auto& r : g_camOriginal)
    {
        *(volatile float*)(r.row + 0x00) = r.dist * distMult;
        *(volatile float*)(r.row + 0x14) = fov > 0 ? fov : r.fov;
    }
    Log("camera: distance x%.2f, fov %.1f", distMult, fov);
    return true;
}

static void RestoreCamera()
{
    for (auto& r : g_camOriginal) { *(volatile float*)(r.row + 0x00) = r.dist; *(volatile float*)(r.row + 0x14) = r.fov; }
    g_camOriginal.clear();
}

static DWORD WINAPI MainThread(LPVOID)
{
    Log("core started");
    Sleep(5000); // let the game finish initialising its singletons after launch
    auto singletons = FindSingletons();
    uintptr_t worldChrMan = singletons.count("WorldChrMan") ? singletons["WorldChrMan"] : 0;
    uintptr_t gameDataMan = StaticFromPattern("48 8B 05 ?? ?? ?? ?? 48 85 C0 74 05 48 8B 40 58 C3 C3");
    Log("%zu singletons; WorldChrMan static %p, GameDataMan static %p", singletons.size(), (void*)worldChrMan, (void*)gameDataMan);
    g_regulationManager = singletons.count("CSRegulationManager") ? singletons["CSRegulationManager"] : 0;

    int lastState = -12345;
    uintptr_t lastPlayer = 1;
    while (g_running)
    {
        Sleep(250);        {
            // camera.txt next to the DLL: "<distance multiplier> <fov degrees>" (re-read live for tuning)
            static float curMult = -1, curFov = -1; float mult = 2.4f, fov = 60.0f;
            if (FILE* cf = _wfopen((g_dir + L"camera.txt").c_str(), L"r")) { fscanf(cf, "%f %f", &mult, &fov); fclose(cf); }
            if ((mult != curMult || fov != curFov) && ApplyCamera(mult, fov)) { curMult = mult; curFov = fov; }
        }
        static int beat = 0; if (++beat % 40 == 0) Log("alive");
        uintptr_t player = 0;                                        // PlayerIns = [[[WorldChrMan]+10EF8]+0]
        Read(Chase(worldChrMan, { 0x10EF8, 0x0 }), player);
        uintptr_t ride = Chase(gameDataMan, { 0x8, 0x8E0, 0x0 });   // RideGameData = [[[GameDataMan]+8]+8E0]
        int state = -1, hp = -1;
        Read(ride + 0x34, state);
        Read(ride + 0x30, hp);
        bool riding = state == 13;
        if (player) SetHidden(player, riding); // enforced every tick: no-op unless the flag differs
        if (player != lastPlayer || state != lastState)
        {
            Log("player %p, torrent state %d, torrent hp %d", (void*)player, state, hp);
            lastPlayer = player; lastState = state;
        }
    }
    if (lastState == 13) SetHidden(lastPlayer, false); // never leave the player invisible after unloading
    RestoreCamera();
    Log("core stopped");
    return 0;
}

extern "C" __declspec(dllexport) void rl_start(const wchar_t* dir)
{
    g_dir = dir;
    g_running = true;
    g_thread = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
}

extern "C" __declspec(dllexport) void rl_stop()
{
    g_running = false;
    if (g_thread) { WaitForSingleObject(g_thread, 5000); CloseHandle(g_thread); g_thread = nullptr; }
    if (g_log) { fclose(g_log); g_log = nullptr; }
}

