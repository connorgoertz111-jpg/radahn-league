// Radahn League loader, loaded by ModEngine2 (external_dlls).
// Hot-reloads rlmod_core.dll from its own folder whenever that file changes, so the core can be rebuilt
// while the game runs. The core is copied to a fresh name before loading, so the original is never locked.
#include <windows.h>
#include <cstdio>
#include <string>

static std::wstring g_dir;

static FILETIME WriteTime(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA a{};
    GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a);
    return a.ftLastWriteTime;
}

static DWORD WINAPI Watch(LPVOID)
{
    std::wstring core = g_dir + L"rlmod_core.dll";
    FILETIME loadedTime{};
    HMODULE live = nullptr;
    int generation = 0;
    for (;;)
    {
        FILETIME t = WriteTime(core);
        if ((t.dwLowDateTime || t.dwHighDateTime) && CompareFileTime(&t, &loadedTime) != 0)
        {
            Sleep(500); // let the compiler finish writing
            if (live)
            {
                if (auto stop = (void (*)())GetProcAddress(live, "rl_stop")) stop();
                FreeLibrary(live);
                live = nullptr;
            }
            std::wstring copy = g_dir + L"live\\rlmod_core_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(++generation) + L".dll";
            CreateDirectoryW((g_dir + L"live").c_str(), nullptr);
            if (CopyFileW(core.c_str(), copy.c_str(), FALSE) && (live = LoadLibraryW(copy.c_str())))
            {
                if (auto start = (void (*)(const wchar_t*))GetProcAddress(live, "rl_start")) start(g_dir.c_str());
            }
            loadedTime = WriteTime(core);
        }
        Sleep(1000);
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(inst);
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(inst, path, MAX_PATH);
        g_dir = path;
        g_dir = g_dir.substr(0, g_dir.find_last_of(L'\\') + 1);
        CreateThread(nullptr, 0, Watch, nullptr, 0, nullptr);
    }
    return TRUE;
}
