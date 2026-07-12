#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <vector>
#include <string>
#include <map>
#include <mutex>
#include <sstream>
#include <iostream>
#include <functional>
#include "MinHook.h"

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "libMinHook-x64-v141-md.lib")

typedef void (*criAtomExPlayer_SetCueName_t)(void* player, void* acb, const char* cueName);
typedef void (*criAtomExPlayer_SetCategoryById_t)(void* player, int id);
typedef void* (*criAtomExAcb_LoadAcbFile_t)(void* acbLib, const char* path, void* work, int workSize, void* buff, int buffSize);
typedef uint32_t (*criAtomExPlayer_Start_t)(void* player);
typedef void (*criAtomExCategory_SetVolume_t)(int id, float vol);
typedef float (*criAtomExCategory_GetVolume_t)(int id);

static criAtomExPlayer_SetCueName_t fpSetCueName = nullptr;
static criAtomExPlayer_SetCategoryById_t fpSetCategoryById = nullptr;
static criAtomExAcb_LoadAcbFile_t fpLoadAcbFile = nullptr;
static criAtomExPlayer_Start_t fpStart = nullptr;
static criAtomExCategory_SetVolume_t fpCategorySetVolume = nullptr;
static criAtomExCategory_GetVolume_t fpCategoryGetVolume = nullptr;

static std::map<void*, std::string> g_playerCueNames;
static std::map<void*, int> g_playerCategoryIds;
static std::map<void*, std::string> g_acbFiles;
static std::map<void*, std::string> g_playerAcbFiles;
static std::mutex g_playerMutex;

static bool SafeReadString(const char* src, char* dest, size_t maxLen) {
    __try {
        if (!src) return false;
        for (size_t i = 0; i < maxLen; ++i) {
            dest[i] = src[i];
            if (src[i] == '\0') return true;
        }
        dest[maxLen - 1] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static std::vector<int> ParseSignature(const std::string& sig) {
    std::vector<int> bytes;
    std::stringstream ss(sig);
    std::string byteStr;
    while (ss >> byteStr) {
        if (byteStr == "??" || byteStr == "?") bytes.push_back(-1);
        else bytes.push_back(std::stoi(byteStr, nullptr, 16));
    }
    return bytes;
}

static uintptr_t PatternScan(HMODULE hModule, const std::string& sig) {
    if (!hModule) return 0;
    MODULEINFO modInfo;
    if (!GetModuleInformation(GetCurrentProcess(), hModule, &modInfo, sizeof(MODULEINFO))) return 0;
    uint8_t* base = (uint8_t*)modInfo.lpBaseOfDll;
    size_t size = modInfo.SizeOfImage;
    std::vector<int> pattern = ParseSignature(sig);
    for (size_t i = 0; i + pattern.size() <= size; ++i) {
        bool found = true;
        for (size_t j = 0; j < pattern.size(); ++j) {
            if (pattern[j] != -1 && base[i + j] != (uint8_t)pattern[j]) { found = false; break; }
        }
        if (found) return (uintptr_t)(base + i);
    }
    return 0;
}

struct CRIHookCallbacks {
    std::function<void(const char* cueName, bool blocked)> onCueName;
    std::function<bool(const char* cueName, int poolId)> onStartBgm;
    std::function<void()> onSeFinish;
    std::function<void()> onNonCustomBgm;
};

static CRIHookCallbacks g_criCallbacks;

static void Hook_SetCueName(void* player, void* acb, const char* cueName) {
    char safeName[256];
    if (SafeReadString(cueName, safeName, sizeof(safeName))) {
        std::lock_guard<std::mutex> lock(g_playerMutex);
        if (g_playerCueNames.size() > 500) { g_playerCueNames.clear(); g_playerAcbFiles.clear(); }
        g_playerCueNames[player] = safeName;
        if (g_acbFiles.count(acb))
            g_playerAcbFiles[player] = g_acbFiles[acb];
        else
            g_playerAcbFiles[player] = "Unknown";
        if (strncmp(safeName, "BGM_", 4) == 0 || strncmp(safeName, "SE_FINISH", 9) == 0) {
            bool blocked = (strcmp(safeName, "BGM_MONSTERTRUCK_01") == 0 || strcmp(safeName, "BGM_TOP_MENU") == 0 ||
                strncmp(safeName, "BGM_CHARASELECT_", 16) == 0 || strncmp(safeName, "BGM_GARAGE_", 11) == 0 ||
                strncmp(safeName, "BGM_PARTYRACE_", 14) == 0 || strncmp(safeName, "BGM_PREVIEW_", 12) == 0 ||
                strncmp(safeName, "BGM_LISTEN_", 11) == 0);
            if (g_criCallbacks.onCueName) g_criCallbacks.onCueName(safeName, blocked);
        }
    }
    if (fpSetCueName) fpSetCueName(player, acb, cueName);
}

static void Hook_SetCategoryById(void* player, int id) {
    { std::lock_guard<std::mutex> lock(g_playerMutex); g_playerCategoryIds[player] = id; }
    if (fpSetCategoryById) fpSetCategoryById(player, id);
}

static void* Hook_LoadAcbFile(void* acbLib, const char* path, void* work, int workSize, void* buff, int buffSize) {
    void* ret = fpLoadAcbFile(acbLib, path, work, workSize, buff, buffSize);
    if (ret && path) { std::lock_guard<std::mutex> lock(g_playerMutex); g_acbFiles[ret] = path; }
    return ret;
}

static uint32_t Hook_Start(void* player) {
    std::string name = "Unknown";
    {
        std::lock_guard<std::mutex> lock(g_playerMutex);
        if (g_playerCueNames.count(player)) name = g_playerCueNames[player];
    }

    if (name.find("SE_FINISH") == 0) {
        if (g_criCallbacks.onSeFinish) g_criCallbacks.onSeFinish();
        if (fpStart) return fpStart(player);
        return 0;
    }

    if (name.find("BGM_") == 0) {
        bool blocked = (name == "BGM_MONSTERTRUCK_01" || name == "BGM_TOP_MENU" ||
            name.find("BGM_CHARASELECT_") == 0 || name.find("BGM_GARAGE_") == 0 ||
            name.find("BGM_PARTYRACE_") == 0 || name.find("BGM_PREVIEW_") == 0 ||
            name.find("BGM_LISTEN_") == 0);
        if (blocked) return 0;

        auto isGpFinal = [&]() -> bool {
            const std::string p = "BGM_GP_";
            const std::string s = "_FINAL_1_2";
            if (name.size() <= p.size() + s.size()) return false;
            if (name.find(p) != 0) return false;
            if (name.rfind(s) != name.size() - s.size()) return false;
            for (size_t i = p.size(); i < name.size() - s.size(); i++)
                if (!isdigit((unsigned char)name[i])) return false;
            return true;
        };
        int poolId = 0;
        if (name.find("BGM_LOBBY_") == 0) poolId = 1;
        else if (name.find("BGM_TITLE_") == 0) poolId = 2;

        bool isCustomBgm = (name == "BGM_LAP1" || name == "BGM_LAP2_FORCE" || isGpFinal() || poolId != 0);

        if (isCustomBgm) {
            if (g_criCallbacks.onStartBgm && g_criCallbacks.onStartBgm(name.c_str(), poolId)) {
                if (fpStart) return fpStart(player);
                return 0;
            }
        } else {
            if (g_criCallbacks.onNonCustomBgm) g_criCallbacks.onNonCustomBgm();
        }
    }

    if (fpStart) return fpStart(player);
    return 0;
}

static bool InitCRIHooks(CRIHookCallbacks callbacks) {
    g_criCallbacks = callbacks;
    HMODULE hGame = GetModuleHandleA(nullptr);
    if (MH_Initialize() != MH_OK) return false;

    auto hook = [&](const char* sig, void* detour, void** original) -> bool {
        uintptr_t addr = PatternScan(hGame, sig);
        if (!addr) return false;
        if (MH_CreateHook((void*)addr, detour, original) != MH_OK) return false;
        MH_EnableHook((void*)addr);
        return true;
    };

    hook("48 89 5C 24 ?? 48 89 6C 24 ?? 56 57 41 56 48 83 EC 20 45 33 F6 49 8B F0",
        (void*)Hook_SetCueName, (void**)&fpSetCueName);
    hook("48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC 50 48 8B D9 8B FA",
        (void*)Hook_SetCategoryById, (void**)&fpSetCategoryById);
    hook("48 89 5C 24 ?? 57 48 83 EC 20 48 8B F9 48 85 C9 75 ?? 44 8D 41 ?? 48 8D 15 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C8 FF",
        (void*)Hook_Start, (void**)&fpStart);
    hook("48 89 5C 24 ?? 48 89 7C 24 ?? 55 48 8D 6C 24 ?? 48 81 EC",
        (void*)Hook_LoadAcbFile, (void**)&fpLoadAcbFile);

    uintptr_t addr;
    addr = PatternScan(hGame, "40 53 48 83 EC 30 48 0F BF D9");
    if (addr) fpCategorySetVolume = (criAtomExCategory_SetVolume_t)addr;
    addr = PatternScan(hGame, "40 53 48 83 EC 20 83 64 24 ?? 00");
    if (addr) fpCategoryGetVolume = (criAtomExCategory_GetVolume_t)addr;

    return fpStart != nullptr;
}

static void CleanupCRIHooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    std::lock_guard<std::mutex> lock(g_playerMutex);
    g_playerCueNames.clear();
    g_playerCategoryIds.clear();
    g_acbFiles.clear();
    g_playerAcbFiles.clear();
}
