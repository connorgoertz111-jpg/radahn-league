// Radahn League core, hot-reloaded by the loader (rl_start / rl_stop).
// Finds the game's managers (FD4 singleton lookup from The Grand Archives' Elden Ring table, plus their
// GameDataMan byte pattern) and logs the player character and Torrent's ride state.
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
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

// Row data of a param by row id (0 if missing). Rows are {id, dataOffset, end} at table+0x40.
static uintptr_t ParamRow(const wchar_t* param, uint64_t id)
{
    uintptr_t t = FindParamTable(param); uint16_t rows = 0;
    if (!t || !Read(t + 0x0A, rows)) return 0;
    for (int i = 0; i < rows; i++)
    {
        uint64_t rid = 0, off = 0;
        if (Read(t + 0x40 + i * 24, rid) && rid == id && Read(t + 0x40 + i * 24 + 8, off)) return t + off;
    }
    return 0;
}

// SpEffectParam: +0x170 vfxId. SpEffectVfxParam: +0x00 midstSfxId, +0x08 initSfxId, +0x24 midstDmyId, +0x26 initDmyId.
static void DumpEffectVisuals(std::initializer_list<int> ids)
{
    for (int id : ids)
    {
        uintptr_t sp = ParamRow(L"SpEffectParam", id); int vfx = -1; Read(sp + 0x170, vfx);
        uintptr_t v = vfx > 0 ? ParamRow(L"SpEffectVfxParam", vfx) : 0;
        int midst = -1, init = -1; short midstDmy = -1, initDmy = -1;
        Read(v + 0x00, midst); Read(v + 0x08, init); Read(v + 0x24, midstDmy); Read(v + 0x26, initDmy);
        Log("effect %d: row %p vfx %d -> midstSfx %d (dmy %d), initSfx %d (dmy %d)", id, (void*)sp, vfx, midst, midstDmy, init, initDmy);
    }
}

// Boost flame setup: repurpose SpEffect 5232 (an unused developer placeholder with no visual) and give it a visual row
// that no other effect references, so nothing else in the game changes. sfx/dummy are overridable via boost_fx.txt.
static int g_flameVfxRow = -1;

static void SetupBoostFlame(int sfxId, short dummyId)
{
    uintptr_t spTable = FindParamTable(L"SpEffectParam"), vfxTable = FindParamTable(L"SpEffectVfxParam");
    uint16_t spRows = 0, vfxRows = 0;
    if (!spTable || !vfxTable || !Read(spTable + 0x0A, spRows) || !Read(vfxTable + 0x0A, vfxRows)) return;
    if (g_flameVfxRow < 0)
    {
        std::vector<int> used;
        for (int i = 0; i < spRows; i++)
        {
            uint64_t off = 0; if (!Read(spTable + 0x40 + i * 24 + 8, off)) continue;
            int v = 0; Read(spTable + off + 0x170, v); used.push_back(v);
            for (int k = 0; k < 7; k++) { Read(spTable + off + 0x18C + k * 4, v); used.push_back(v); }
        }
        for (int i = vfxRows - 1; i >= 0 && g_flameVfxRow < 0; i--) // prefer high, rarely touched ids
        {
            uint64_t id = 0; Read(vfxTable + 0x40 + i * 24, id);
            if (id > 0 && std::find(used.begin(), used.end(), (int)id) == used.end()) g_flameVfxRow = (int)id;
        }
        Log("boost flame: using free visual row %d", g_flameVfxRow);
    }
    uintptr_t vfx = ParamRow(L"SpEffectVfxParam", g_flameVfxRow), sp = ParamRow(L"SpEffectParam", 5232);
    if (!vfx || !sp) return;
    *(volatile int*)(vfx + 0x00) = sfxId;     // midstSfxId: looping visual while active
    *(volatile short*)(vfx + 0x24) = dummyId; // midstDmyId: marker it is pinned to
    *(volatile int*)(sp + 0x170) = g_flameVfxRow;
    Log("boost flame: effect 5232 -> visual row %d, sfx %d at marker %d", g_flameVfxRow, sfxId, dummyId);
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

// --- Finding Torrent (diagnostic) ---
// ChrIns +0x190 = module bag; bag +0x68 = physics module (+0x70 position, +0x54 orientation); bag +0xE8 = ride module.
struct Vec3 { float x, y, z; };
static bool ChrPosition(uintptr_t chr, Vec3& p)
{
    uintptr_t bag = 0, phys = 0;
    return Read(chr + 0x190, bag) && bag && Read(bag + 0x68, phys) && phys && Read(phys + 0x70, p);
}

static void SearchHorseMatrices(uintptr_t horse);
static void SearchBoneMatrices(uintptr_t horse);
static void SearchTorrentPose(uintptr_t horse);
static std::atomic<uintptr_t> g_poseArray{ 0 }; // candidate Torrent pose (Havok QS transforms)
static std::atomic<uintptr_t> g_levelHorse{ 0 }; // Torrent ChrIns while riding (see LevelThread)
static void FindHorse(uintptr_t player)
{
    Vec3 me{}; if (!ChrPosition(player, me)) { Log("ride scan: no player position"); return; }
    uintptr_t bag = 0, ride = 0;
    if (!Read(player + 0x190, bag) || !Read(bag + 0xE8, ride) || !ride) { Log("ride scan: no ride module"); return; }
    Log("ride scan: player at %.2f %.2f %.2f, ride module %p", me.x, me.y, me.z, (void*)ride);
    for (uintptr_t off = 0; off < 0x400; off += 8)
    {
        uintptr_t q = 0; if (!Read(ride + off, q) || q < 0x10000) continue;
        for (uintptr_t inner : { (uintptr_t)0, (uintptr_t)8, (uintptr_t)0x10 })
        {
            uintptr_t chr = q; if (inner && (!Read(q + inner, chr) || chr < 0x10000)) continue;
            Vec3 p{}; if (!ChrPosition(chr, p) || chr == player) continue;
            float dx = p.x - me.x, dy = p.y - me.y, dz = p.z - me.z;
            if (dx * dx + dy * dy + dz * dz < 16) Log("ride scan: candidate [ride+%llX]%s -> chr %p at %.2f %.2f %.2f", (unsigned long long)off, inner ? "+deref" : "", (void*)chr, p.x, p.y, p.z);
            if (off == 0x1E8 && !inner) { g_levelHorse = chr; Log("leveling Torrent %p", (void*)chr); SearchTorrentPose(chr); }
        }
    }
}

// --- Torrent transform search (diagnostic) ---
// Looks for 4x4 float matrices (row-major, translation in the last row) whose translation is Torrent's position,
// in Torrent's ChrIns and in every object it points to. Logs each with its "up" row so pitch/roll show up.
static std::vector<std::pair<std::string, uintptr_t>> g_horseMatrices;

static void SearchHorseMatrices(uintptr_t horse)
{
    Vec3 pos{}; if (!ChrPosition(horse, pos)) return;
    g_horseMatrices.clear();
    auto scan = [&](uintptr_t base, const std::string& name) {
        for (uintptr_t o = 0x30; o < 0x800; o += 4)
        {
            Vec3 t{}; if (!Read(base + o, t)) break;
            if (fabsf(t.x - pos.x) > 0.05f || fabsf(t.y - pos.y) > 0.6f || fabsf(t.z - pos.z) > 0.05f) continue;
            float m[12]; if (!Read(base + o - 0x30, m)) continue;
            float up2 = m[4] * m[4] + m[5] * m[5] + m[6] * m[6], r2 = m[0] * m[0] + m[1] * m[1] + m[2] * m[2];
            if (fabsf(up2 - 1) > 0.05f || fabsf(r2 - 1) > 0.05f) continue; // not a rotation
            char key[96]; snprintf(key, sizeof key, "%s+%llX", name.c_str(), (unsigned long long)(o - 0x30));
            g_horseMatrices.push_back({ key, base + o - 0x30 });
        }
    };
    scan(horse, "chr");
    for (uintptr_t o = 0; o < 0x600; o += 8)
    {
        uintptr_t p = 0; if (!Read(horse + o, p) || p < 0x10000) continue;
        char n[32]; snprintf(n, sizeof n, "[chr+%llX]", (unsigned long long)o); scan(p, n);
    }
    uintptr_t bag = 0; Read(horse + 0x190, bag);
    for (uintptr_t o = 0; bag && o < 0x200; o += 8)
    {
        uintptr_t p = 0; if (!Read(bag + o, p) || p < 0x10000) continue;
        char n[32]; snprintf(n, sizeof n, "[bag+%llX]", (unsigned long long)o); scan(p, n);
    }
    Log("matrix search: %zu candidates", g_horseMatrices.size());
}

static void LogHorseMatrices()
{
    for (auto& [name, at] : g_horseMatrices)
    {
        float m[12]; if (!Read(at, m)) continue;
        // up vector = row 1; pitch/roll in degrees from how far "up" leans
        float pitch = asinf(fmaxf(-1, fminf(1, m[6]))) * 57.3f, roll = asinf(fmaxf(-1, fminf(1, m[4]))) * 57.3f;
        Log("  %s up=(%.2f %.2f %.2f) pitch %.0f roll %.0f", name.c_str(), m[4], m[5], m[6], pitch, roll);
    }
}

// --- Game "special effects" (SpEffect) apply/remove: byte patterns as documented by The Grand Archives' table ---
using AddSpEffectFn = void* (*)(uintptr_t chrIns, int id, int unk);
using RemoveSpEffectFn = void* (*)(uintptr_t specialEffect, int id);
static AddSpEffectFn g_addSpEffect;
static RemoveSpEffectFn g_removeSpEffect;

static void FindSpEffectFunctions()
{
    auto add = Scan(Section(".text"), "0f 28 0d ?? ?? ?? ?? ?? 8d ?? ?? 0f 29 ?? ?? ?? 0f b6 d8");
    auto rem = Scan(Section(".text"), "48 83 EC 28 8B C2 48 8B 51 08 48 85 D2 ?? ?? 90");
    if (add.size() == 1) g_addSpEffect = (AddSpEffectFn)(add[0] - 0x1D);
    if (rem.size() == 1) g_removeSpEffect = (RemoveSpEffectFn)rem[0];
    Log("AddSpEffect %p, RemoveSpEffect %p", (void*)g_addSpEffect, (void*)g_removeSpEffect);
}

static void AddEffect(uintptr_t chr, int id) { if (chr && id && g_addSpEffect) g_addSpEffect(chr, id, 1); }
static void RemoveEffect(uintptr_t chr, int id)
{
    uintptr_t se = 0;
    if (chr && id && g_removeSpEffect && Read(chr + 0x178, se) && se) g_removeSpEffect(se, id);
}

// Boost flame: effect applied to Torrent while boosting (id overridable live via boost_fx.txt)
static std::atomic<int> g_boostFx{ 5232 };

// --- Bone matrix search (diagnostic, for spinning wheels) ---
// Breadth-first walk of pointers reachable from Torrent's ChrIns (3 levels), looking for long runs of affine matrices:
// 4x4 (64 bytes, last column 0,0,0,1) or 3x4 (48 bytes, orthonormal 3x3). Torrent has 366 bones.
static bool LooksAffine64(const float* m)
{
    if (fabsf(m[3]) > 1e-4f || fabsf(m[7]) > 1e-4f || fabsf(m[11]) > 1e-4f || fabsf(m[15] - 1) > 1e-4f) return false;
    for (int r = 0; r < 3; r++) { float l = m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2]; if (l < 0.001f || l > 100) return false; }
    for (int i = 0; i < 16; i++) if (!std::isfinite(m[i])) return false;
    return true;
}

// Havok hkQsTransform: translation(4) rotation quaternion(4) scale(4) = 48 bytes
static bool LooksQs(const float* t)
{
    for (int i = 0; i < 12; i++) if (!std::isfinite(t[i])) return false;
    float q = t[4] * t[4] + t[5] * t[5] + t[6] * t[6] + t[7] * t[7];
    if (fabsf(q - 1) > 0.01f) return false;
    if (fabsf(t[8] - 1) > 0.2f || fabsf(t[9] - 1) > 0.2f || fabsf(t[10] - 1) > 0.2f) return false;
    return fabsf(t[0]) < 100 && fabsf(t[1]) < 100 && fabsf(t[2]) < 100;
}
// Row-major 3x4: three rows of (rotation row, translation) = 48 bytes
static bool LooksAffine48(const float* m)
{
    for (int i = 0; i < 12; i++) if (!std::isfinite(m[i])) return false;
    for (int r = 0; r < 3; r++) { float l = m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2]; if (fabsf(l - 1) > 0.05f) return false; }
    return true;
}

// Torrent signature: local bone offsets from his skeleton (c8000.flver), used to recognise his pose array
static const float kTorrentOffsets[][3] = {
    { 0.0f, 1.1409f, 0.5735f }, { -0.0696f, 0.2002f, 0.6515f }, { -0.0256f, 0.2002f, 0.9567f }, { 0.0f, 1.1633f, 0.4744f },
    { 0.2904f, 0.0f, 0.0f }, { 0.2423f, 0.2024f, 0.3677f }, { 0.0990f, 0.0f, -0.0223f } };

static int TorrentMatches(uintptr_t qsArray, int count)
{
    int hits = 0;
    for (int k = 0; k < count; k++)
    {
        float t[3]; if (!Read(qsArray + k * 48, t)) break;
        for (auto& s : kTorrentOffsets)
            if (fabsf(fabsf(t[0]) - fabsf(s[0])) < 0.003f && fabsf(fabsf(t[1]) - fabsf(s[1])) < 0.003f && fabsf(fabsf(t[2]) - fabsf(s[2])) < 0.003f) { hits++; break; }
    }
    return hits;
}

static void SearchTorrentPose(uintptr_t horse)
{
    // Direct path found by the earlier broad search: [[[anim+0x15A8]+0x10]+0x118] (123 QS transforms, 5 Torrent offsets)
    {
        uintptr_t bag0 = 0, anim0 = 0;
        if (Read(horse + 0x190, bag0) && Read(bag0 + 0x28, anim0) && anim0)
        {
            uintptr_t arr = 0; int qs = 0; float m[12];
            for (int tries = 0; tries < 40 && g_running && qs < 20; tries++)
            {
                arr = 0; qs = 0; Read(Chase(anim0 + 0x15A8, { 0x10, 0x118 }), arr);
                while (arr && qs < 400 && Read(arr + qs * 48, m) && LooksQs(m)) qs++;
                if (qs < 20) Sleep(250);
            }
            Log("pose direct: %p, %d QS, %d Torrent matches", (void*)arr, qs, arr ? TorrentMatches(arr, qs) : 0);
            g_poseArray = arr;
            if (arr) if (FILE* df = _wfopen((g_dir + L"pose_dump.txt").c_str(), L"w"))
            {
                for (int k = 0; k < qs + 40; k++)
                {
                    float t[12] = {}; Read(arr + k * 48, t);
                    fprintf(df, "%d %.4f %.4f %.4f | %.3f %.3f %.3f %.3f | %.2f %.2f %.2f\n", k, t[0], t[1], t[2], t[4], t[5], t[6], t[7], t[8], t[9], t[10]);
                }
                fclose(df);
            }
            return;
        }
    }
    uintptr_t bag = 0, anim = 0;
    if (!Read(horse + 0x190, bag) || !Read(bag + 0x28, anim) || !anim) { Log("pose search: no anim module"); return; }
    std::vector<std::pair<uintptr_t, std::string>> frontier{ { anim, "anim" } }, next;
    std::vector<uintptr_t> seen{ anim };
    for (int depth = 0; depth < 4; depth++)
    {
        for (auto& [node, path] : frontier)
            for (uintptr_t o = 0; o < 0x2000 && g_running; o += 8)
            {
                uintptr_t p = 0;
                if (!Read(node + o, p) || p < 0x10000000000ull || p > 0x7FFFFFFFFFFFull || (p & 0xF)) continue;
                if (std::find(seen.begin(), seen.end(), p) != seen.end()) continue;
                seen.push_back(p);
                char childPath[200]; snprintf(childPath, sizeof childPath, "%s[+%llX]", path.c_str(), (unsigned long long)o);
                int qs = 0; float m[12];
                while (qs < 2000 && Read(p + qs * 48, m) && LooksQs(m)) qs++;
                if (qs >= 20) { int hits = TorrentMatches(p, qs); if (hits >= 2) { Log("pose? %s -> %p: %d QS, %d Torrent matches", childPath, (void*)p, qs, hits);
                    if (hits >= 4)
                        if (FILE* df = _wfopen((g_dir + L"pose_dump.txt").c_str(), L"w"))
                        {
                            for (int k = 0; k < qs + 40; k++)
                            {
                                float t[12] = {}; Read(p + k * 48, t);
                                fprintf(df, "%d %.4f %.4f %.4f | %.3f %.3f %.3f %.3f | %.2f %.2f %.2f\n", k, t[0], t[1], t[2], t[4], t[5], t[6], t[7], t[8], t[9], t[10]);
                            }
                            fclose(df);
                        }
                } }
                if (seen.size() < 40000 && depth < 3) next.push_back({ p, childPath });
            }
        frontier.swap(next); next.clear();
    }
    Log("pose search done (%zu nodes)", seen.size());
}

static void SearchBoneMatrices(uintptr_t horse)
{
    std::vector<std::pair<uintptr_t, std::string>> frontier{ { horse, "chr" } }, next;
    std::vector<uintptr_t> seen{ horse };
    int reported = 0;
    for (int depth = 0; depth < 3 && reported < 20; depth++)
    {
        for (auto& [node, path] : frontier)
        {
            if (!g_running) break;
            for (uintptr_t o = 0; o < 0x800; o += 8)
            {
                uintptr_t p = 0;
                if (!Read(node + o, p) || p < 0x10000000000ull || p > 0x7FFFFFFFFFFFull || (p & 0xF)) continue;
                if (std::find(seen.begin(), seen.end(), p) != seen.end()) continue;
                seen.push_back(p);
                char childPath[160]; snprintf(childPath, sizeof childPath, "%s[+%llX]", path.c_str(), (unsigned long long)o);
                // count consecutive 4x4 affine matrices at p
                int run = 0, qs = 0, a48 = 0; float m[16];
                while (run < 2000 && Read(p + run * 64, m) && LooksAffine64(m)) run++;
                while (qs < 2000 && Read(p + qs * 48, m) && LooksQs(m)) qs++;
                while (a48 < 2000 && Read(p + a48 * 48, m) && LooksAffine48(m)) a48++;
                int best = std::max({ run, qs, a48 });
                if (best >= 60 && reported < 30) { Log("bones? %s -> %p: 4x4 %d, qs %d, 3x4 %d", childPath, (void*)p, run, qs, a48); reported++; }
                if (qs >= 60)
                    if (FILE* df = _wfopen((g_dir + L"qs_dump.txt").c_str(), L"a"))
                    {
                        fprintf(df, "ARRAY %s %p %d\n", childPath, (void*)p, qs);
                        for (int k = 0; k < qs; k++) { float t[12]; Read(p + k * 48, t); fprintf(df, "%d %.4f %.4f %.4f\n", k, t[0], t[1], t[2]); }
                        fclose(df);
                    }
                if (seen.size() < 20000) next.push_back({ p, childPath });
            }
        }
        frontier.swap(next); next.clear();
    }
    Log("bone search done (%zu nodes visited)", seen.size());
}

// --- Ball: projectiles (Bullet) spawned through CSBulletManager ---
// Spawn function byte pattern and argument layout as documented by The Grand Archives' table:
//   fn(CSBulletManager*, out handle*, params*, unk*); params +0x14 = bullet id, +0x80 = x, y, z.
using SpawnBulletFn = void* (*)(uintptr_t manager, void* outHandle, void* params, void* unk);
static SpawnBulletFn g_spawnBullet;
static uintptr_t g_bulletManager; // static address holding the CSBulletManager instance

static void FindBulletSpawn(std::unordered_map<std::string, uintptr_t>& singletons)
{
    auto hits = Scan(Section(".text"), "40 53 55 56 57 48 81 EC 98 07 00 00 48 C7 44 24 50 FE FF FF FF");
    if (hits.size() == 1) g_spawnBullet = (SpawnBulletFn)hits[0];
    g_bulletManager = singletons.count("CSBulletManager") ? singletons["CSBulletManager"] : 0;
    Log("SpawnBullet %p (%zu hits), CSBulletManager static %p", (void*)g_spawnBullet, hits.size(), (void*)g_bulletManager);
}

static bool SpawnBullet(int id, Vec3 at)
{
    uintptr_t manager = 0;
    if (!g_spawnBullet || !Read(g_bulletManager, manager) || !manager) return false;
    alignas(16) uint8_t mem[0x200] = {};
    *(int*)(mem + 0x14) = id;
    *(Vec3*)(mem + 0x80) = at;
    g_spawnBullet(manager, mem + 0x180, mem, mem + 0x1C0);
    Log("spawned bullet %d at %.1f %.1f %.1f -> handle %08X", id, at.x, at.y, at.z, *(uint32_t*)(mem + 0x180));
    return *(uint32_t*)(mem + 0x180) != 0xFFFFFFFF;
}

// --- Rocket League sounds, decoded from the player's own Rocket League install ---
// Nothing from Rocket League ships with the mod: on first launch vgmstream (bundled, ISC license) decodes two
// sounds from the player's Rocket League banks into a local cache, and the add-on plays them from there.
#include <mmsystem.h>
#include <shlobj.h>

struct RlSound { const wchar_t* file; const wchar_t* bank; const char* streamName; };
static const RlSound kHit{ L"ball_hit.wav", L"SFX_Ball.bnk", "139746051" };
static const RlSound kGoal{ L"goal.wav", L"SFX_GoalEvent.bnk", "719189024" };
static std::wstring g_soundDir;

static std::wstring RocketLeagueDir()
{
    wchar_t buf[MAX_PATH];
    if (GetEnvironmentVariableW(L"RADAHN_LEAGUE_RL_DIR", buf, MAX_PATH)) return buf; // set by Melty at launch
    return L"C:\\Program Files\\Epic Games\\rocketleague";
}

// Runs vgmstream-cli hidden and returns its stdout
static std::string RunVgmstream(const std::wstring& args)
{
    std::wstring cmd = L"\"" + g_dir + L"vgmstream\\vgmstream-cli.exe\" " + args;
    SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
    HANDLE rd, wr; CreatePipe(&rd, &wr, &sa, 0); SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{ sizeof si }; si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE; si.hStdOutput = wr; si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::string out;
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        CloseHandle(wr); wr = nullptr;
        char chunk[4096]; DWORD got;
        while (ReadFile(rd, chunk, sizeof chunk, &got, nullptr) && got) out.append(chunk, got);
        WaitForSingleObject(pi.hProcess, 30000); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
    if (wr) CloseHandle(wr);
    CloseHandle(rd);
    return out;
}

static bool PrepareSound(const RlSound& s)
{
    std::wstring target = g_soundDir + s.file;
    if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    std::wstring bank = RocketLeagueDir() + L"\\TAGame\\CookedPCConsole\\" + s.bank;
    if (GetFileAttributesW(bank.c_str()) == INVALID_FILE_ATTRIBUTES) { Log("sound: Rocket League bank not found: %ls", bank.c_str()); return false; }
    // find the subsong by its Wwise id (robust to the bank's order changing between Rocket League updates)
    for (int sub = 1; sub <= 64; sub++)
    {
        std::string meta = RunVgmstream(L"-m -s " + std::to_wstring(sub) + L" \"" + bank + L"\"");
        if (meta.find("stream name: ") == std::string::npos) break;
        if (meta.find(std::string("stream name: ") + s.streamName) == std::string::npos) continue;
        RunVgmstream(L"-s " + std::to_wstring(sub) + L" -o \"" + target + L"\" \"" + bank + L"\"");
        bool ok = GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES;
        Log("sound: decoded %ls (subsong %d) -> %s", s.file, sub, ok ? "ok" : "failed");
        return ok;
    }
    Log("sound: stream %s not found in %ls", s.streamName, s.bank);
    return false;
}

static void PrepareSounds()
{
    wchar_t* local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) { g_soundDir = std::wstring(local) + L"\\RadahnLeague\\sounds\\"; CoTaskMemFree(local); }
    SHCreateDirectoryExW(nullptr, g_soundDir.c_str(), nullptr);
    PrepareSound(kHit); PrepareSound(kGoal);
}

static void PlayRl(const RlSound& s) { PlaySoundW((g_soundDir + s.file).c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT); }

// --- Boost: hold B (controller) or Space (keyboard) while riding ---
// Torrent's movement comes from his animations, so boosting plays them faster.
// Animation speed = [[ChrIns+0x190]+0x28]+0x17C8 (float, 1 = normal).
#include <xinput.h>
static float g_boostRate = 2.2f; // overridable live via boost.txt next to the DLL
static std::atomic<bool> g_boosting{ false };

static bool BoostHeld()
{
    XINPUT_STATE s{};
    for (DWORD pad = 0; pad < 4; pad++)
        if (XInputGetState(pad, &s) == ERROR_SUCCESS && (s.Gamepad.wButtons & XINPUT_GAMEPAD_B)) return true;
    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid); // keyboard only while the game is in front
    return pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_SPACE) & 0x8000);
}

static void SetAnimSpeed(uintptr_t chr, float rate)
{
    uintptr_t bag = 0, anim = 0;
    if (chr && Read(chr + 0x190, bag) && bag && Read(bag + 0x28, anim) && anim)
    {
        float cur = 0;
        if (Read(anim + 0x17C8, cur) && cur != rate) *(volatile float*)(anim + 0x17C8) = rate;
    }
}

// --- Keeping the car level ---
// Torrent's ChrIns+0x58 object holds two 4x4 transforms: +0x1B0 stays level (logical placement) and +0x230 is the
// drawn model transform, which the game tilts with the gallop and the ground. While riding, a fast loop copies the
// level rotation into the drawn transform (keeping its position), so the car stays flat.

static HANDLE g_levelThread;

static DWORD WINAPI LevelThread(LPVOID)
{
    timeBeginPeriod(1);
    while (g_running)
    {
        uintptr_t horse = g_levelHorse;
        uintptr_t ctrl = 0;
        if (horse && Read(horse + 0x58, ctrl) && ctrl)
        {
            float level[12];
            if (Read(ctrl + 0x1B0, level)) memcpy((void*)(ctrl + 0x230), level, sizeof level); // rotation rows only
        }
        static uintptr_t boostedHorse = 0;
        bool boost = horse && BoostHeld();
        static uintptr_t fxHorse = 0; static int fxId = 0;
        if (boost != g_boosting)
        {
            g_boosting = boost;
            if (boost) { fxHorse = horse; fxId = g_boostFx; AddEffect(fxHorse, fxId); }
            else { RemoveEffect(fxHorse, fxId); fxHorse = 0; }
            Log(boost ? "boost on (fx %d)" : "boost off", fxId);
        }
        if (horse) { SetAnimSpeed(horse, boost ? g_boostRate : 1.0f); boostedHorse = horse; }
        else if (boostedHorse) { boostedHorse = 0; }
        Sleep(1);
    }
    timeEndPeriod(1);
    return 0;
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
    FindSpEffectFunctions();
    FindBulletSpawn(singletons);
    PrepareSounds();
    for (int tries = 0; tries < 60 && !FindParamTable(L"SpEffectParam"); tries++) Sleep(500);
    DumpEffectVisuals({ 5232, 415, 416, 460, 1776, 1627, 3160, 1630000, 1632000, 1703000 });
    for (int b : { 2500210, 2500220, 4040170, 4510330, 8110000, 8110001, 8110010, 10611000 }) { int sfx = -1; Read(ParamRow(L"Bullet", b) + 0x04, sfx); Log("bullet %d visual %d", b, sfx); }

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
        if (FILE* bf = _wfopen((g_dir + L"boost.txt").c_str(), L"r")) { float r = 0; if (fscanf(bf, "%f", &r) == 1 && r >= 1 && r <= 5 && r != g_boostRate) { g_boostRate = r; Log("boost rate %.2f", r); } fclose(bf); }
        { static int curSfx = 0, curDmy = 0; int sfx = 625015, dmy = 900; // Crucible Knight fire breath at the exhaust marker added to the car model
          if (FILE* ff = _wfopen((g_dir + L"boost_fx.txt").c_str(), L"r")) { fscanf(ff, "%d %d", &sfx, &dmy); fclose(ff); }
          if (sfx != curSfx || dmy != curDmy) { SetupBoostFlame(sfx, (short)dmy); curSfx = sfx; curDmy = dmy; } }
        if (uintptr_t pa = g_poseArray; pa && lastState == 13) { static int tick = 0; if (++tick % 4 == 0) { float a[12], b[12], c[12]; Read(pa + 1 * 48, a); Read(pa + 20 * 48, b); Read(pa + 60 * 48, c); Log("pose sample: [1] q %.3f %.3f %.3f %.3f  [20] q %.3f %.3f %.3f %.3f  [60] t %.3f %.3f %.3f", a[4], a[5], a[6], a[7], b[4], b[5], b[6], b[7], c[0], c[1], c[2]); } }
        {   // test keys until the ball exists: F9 = ball hit, F10 = goal (only while the game is in front)
            DWORD fpid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &fpid);
            static bool f9 = false, f10 = false;
            bool n9 = fpid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F9) & 0x8000), n10 = fpid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F10) & 0x8000);
            if (n9 && !f9) { PlayRl(kHit); Log("played ball hit"); }
            if (n10 && !f10) { PlayRl(kGoal); Log("played goal"); }
            static bool f8 = false; bool n8 = fpid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F8) & 0x8000);
            if (n8 && !f8 && g_levelHorse) { int id = 3850310; if (FILE* bf2 = _wfopen((g_dir + L"ball_test.txt").c_str(), L"r")) { fscanf(bf2, "%d", &id); fclose(bf2); } Vec3 p{}; if (ChrPosition(g_levelHorse, p)) { p.y += 3; SpawnBullet(id, p); } }
            f8 = n8;
            f9 = n9; f10 = n10;
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
        static bool scanned = false;
        if (!riding) { scanned = false; g_levelHorse = 0; }
        else if (player && !scanned) { FindHorse(player); scanned = true; }
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
    g_levelThread = CreateThread(nullptr, 0, LevelThread, nullptr, 0, nullptr);
}

extern "C" __declspec(dllexport) void rl_stop()
{
    g_running = false;
    if (g_thread) { WaitForSingleObject(g_thread, INFINITE); CloseHandle(g_thread); g_thread = nullptr; }
    if (g_levelThread) { WaitForSingleObject(g_levelThread, 2000); CloseHandle(g_levelThread); g_levelThread = nullptr; }
    if (g_log) { fclose(g_log); g_log = nullptr; }
}

