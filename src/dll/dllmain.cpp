#define NOMINMAX
#include <windows.h>
#include <thread>
#include <atomic>
#include <iostream>
#include <cstdio>
#include <fstream>
#include <string>
#include <chrono>
#include "cri_hooks.h"
#include "../shared/status.h"

static std::atomic<bool> g_running{ false };
static HMODULE g_hModule = nullptr;
static SharedMemory* g_shared = nullptr;
static HANDLE g_sharedMapping = nullptr;
static std::atomic<bool> g_bgmActive{ false };
static std::atomic<bool> g_playedFinish{ false };
static int g_activePool = 0;
static float g_originalBgmVolume = 1.0f;
static bool g_playNewMusic = false;

static std::string WideToUtf8(const wchar_t* w) {
    if (!w || !*w) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], len, nullptr, nullptr);
    s.resize(len - 1);
    return s;
}

static std::string GetDllDir() {
    wchar_t wPath[MAX_PATH];
    if (GetModuleFileNameW(g_hModule, wPath, MAX_PATH) == 0) return ".";
    std::string s = WideToUtf8(wPath);
    size_t pos = s.find_last_of("\\/");
    return (pos != std::string::npos) ? s.substr(0, pos) : ".";
}

static void WriteCommand(int cmd, const char* cueName, int poolId, bool allowLoop) {
    if (!g_shared) return;
    while (InterlockedCompareExchange(&g_shared->cmdReady, 1, 0) != 0)
        Sleep(1);
    g_shared->command = cmd;
    strncpy_s(g_shared->cueName, cueName, sizeof(g_shared->cueName) - 1);
    g_shared->poolId = poolId;
    g_shared->allowLoop = allowLoop;
    InterlockedExchange(&g_shared->cmdReady, 1);
}

static void InitThread() {
    AllocConsole();
    FILE* f;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    std::cout << "[Hook] DLL loaded, initializing..." << std::endl;

    for (int i = 0; i < 20; i++) {
        g_sharedMapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, SHARED_MEMORY_NAME);
        if (g_sharedMapping)
            g_shared = (SharedMemory*)MapViewOfFile(g_sharedMapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedMemory));
        if (g_shared) break;
        std::cerr << "[Hook] Waiting for shared memory... (" << (i+1) << "/20)" << std::endl;
        Sleep(500);
    }
    if (!g_shared) {
        std::cerr << "[Hook] Failed to open shared memory after retries. Is the EXE running?" << std::endl;
        return;
    }

    std::string dllDir = GetDllDir();
    std::ifstream settingsFile(dllDir + "\\settings.txt");
    if (settingsFile.is_open()) {
        std::string line;
        while (std::getline(settingsFile, line)) {
            if (line.find("PlayNewMusic:") != std::string::npos)
                g_playNewMusic = (line.find("true") != std::string::npos);
        }
    }

    CRIHookCallbacks cb;
    cb.onCueName = [](const char* name, bool blocked) {
        if (!blocked) std::cout << "[Hook] CRI Cue: " << name << std::endl;
    };

    cb.onStartBgm = [](const char* cueName, int poolId) -> bool {
        bool hasTracks = false;
        if (poolId == 0 && g_shared->hasBgmTracks) hasTracks = true;
        if (poolId == 1 && g_shared->hasLobbyTracks) hasTracks = true;
        if (poolId == 2 && g_shared->hasTitleTracks) hasTracks = true;
        if (!hasTracks) return false;
        if (poolId == g_activePool && g_bgmActive.load()) return true;

        if (fpCategorySetVolume) {
            float curVol = fpCategoryGetVolume ? fpCategoryGetVolume(0) : 1.0f;
            if (curVol > 0.0f) g_originalBgmVolume = curVol;
            fpCategorySetVolume(0, 0.0f);
        }
        g_activePool = poolId;
        g_playedFinish.store(false);
        g_bgmActive.store(true);
        WriteCommand(1, cueName, poolId, !g_playNewMusic);
        std::cout << "[Hook] " << cueName << " -> pool " << poolId << " (custom BGM)" << std::endl;
        return true;
    };

    cb.onSeFinish = []() {
        bool expected = false;
        if (g_playedFinish.compare_exchange_strong(expected, true)) {
            if (fpCategorySetVolume) fpCategorySetVolume(0, g_originalBgmVolume);
            g_activePool = 0;
            g_bgmActive.store(false);
            WriteCommand(3, "SE_FINISH", 0, false);
            std::cout << "[Hook] SE_FINISH -> Custom BGM stopped" << std::endl;
        }
    };

    cb.onNonCustomBgm = []() {
        if (g_bgmActive.load()) {
            if (fpCategorySetVolume) fpCategorySetVolume(0, g_originalBgmVolume);
            g_bgmActive.store(false);
            g_activePool = 0;
            WriteCommand(2, "STOP_CUSTOM", 0, false);
            std::cout << "[Hook] Non-custom BGM detected -> Stopping custom playback" << std::endl;
        }
    };

    if (!InitCRIHooks(cb))
        std::cerr << "[Hook] Warning: Some CRI hooks failed" << std::endl;
    else
        std::cout << "[Hook] CRI hooks installed" << std::endl;

    g_shared->active = true;
    std::cout << "[Hook] Ready! Waiting for BGM cues..." << std::endl;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    if (g_shared) g_shared->active = false;
    std::cout << "[Hook] Shutting down." << std::endl;
}

extern "C" __declspec(dllexport) void __cdecl UninstallMod() {}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        g_running = true;
        std::thread(InitThread).detach();
        break;
    case DLL_PROCESS_DETACH:
        g_running = false;
        CleanupCRIHooks();
        if (g_shared) { g_shared->active = false; UnmapViewOfFile(g_shared); g_shared = nullptr; }
        if (g_sharedMapping) { CloseHandle(g_sharedMapping); g_sharedMapping = nullptr; }
        break;
    }
    return TRUE;
}
