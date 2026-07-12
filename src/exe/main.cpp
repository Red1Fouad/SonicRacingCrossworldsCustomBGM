#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <xaudio2.h>
#include <d3d9.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <random>
#include <fstream>
#include <sstream>
#include <thread>
#include <atomic>
#include <chrono>
#include <algorithm>
#define DR_MP3_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#include "audio_engine.h"
#include "injector.h"
#include "../shared/status.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx9.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "SDL2.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static const char* WINDOW_TITLE = "Sonic Custom BGM";
static const int WIN_W = 720;
static const int WIN_H = 740;
#define WM_APP_GONE (WM_APP + 2)
#define WM_TRAYICON (WM_APP + 1)
#define TRAY_ICON_ID 1
#define TRAY_RESTORE 1001
#define TRAY_PLAYPAUSE 1002
#define TRAY_SKIP 1003
#define TRAY_EXIT 1004

static HWND g_hWnd = nullptr;

static IDirect3D9* g_pD3D = nullptr;
static IDirect3DDevice9* g_pd3dDevice = nullptr;
static D3DPRESENT_PARAMETERS g_d3dpp = {};

static ImFont* g_fontRegular = nullptr;
static ImFont* g_fontBold = nullptr;
static ImFont* g_fontMono = nullptr;

static AudioEngine g_audio;
static std::vector<CompressedAudio> g_bgmPool, g_lobbyPool, g_titlePool;
static std::vector<int> g_bgmOrder, g_lobbyOrder, g_titleOrder;
static int g_bgmOrderPos = 0, g_lobbyOrderPos = 0, g_titleOrderPos = 0;
static int g_activePool = 0;
static std::atomic<bool> g_bgmActive{ false };
static std::atomic<bool> g_playedFinish{ false };
static bool g_playNewMusic = false;
static bool g_muteOnUnfocus = false;
static std::atomic<float> g_customBgmVolume{ 1.0f };
static std::atomic<bool> g_paused{ false };
static std::mt19937 g_rng((unsigned)std::random_device{}());
static std::atomic<bool> g_audioInitialized{ false };
static std::atomic<bool> g_injected{ false };
static std::atomic<bool> g_connecting{ false };
static std::atomic<bool> g_scanRunning{ true };
static std::atomic<bool> g_audioThreadRunning{ true };
static DWORD g_targetPid = 0;
static HANDLE g_hSharedMap = nullptr;
static SharedMemory* g_pShared = nullptr;
static std::mutex g_trackNameMutex;
static std::string g_currentTrackName;

static bool g_showProcessPopup = false;
static std::vector<ProcessInfo> g_processList;
static bool g_gameGone = false;

static std::set<std::string> g_favorites;
static std::string g_searchFilter;
static char g_searchBuf[256] = "";
static bool g_favoritesOnly = false;
static int g_themeIndex = 0;
static bool g_minimizeToTray = false;
static bool g_autostartEnabled = false;
static bool g_showSettingsPopup = false;
static bool g_showCreditsPopup = false;
static bool g_trayIconVisible = false;
static NOTIFYICONDATAA g_nid = {};
static HICON g_trayIcon = nullptr;
static std::string g_lastNotifiedTrack;
static std::atomic<bool> g_settingsLoaded{ false };

struct RecentEntry { std::string filename; int poolIndex; };
static std::vector<RecentEntry> g_recentTracks;
static const int MAX_RECENT = 50;

enum HotkeyAction { HOTKEY_SKIP = 0, HOTKEY_PREV, HOTKEY_VOLUP, HOTKEY_VOLDOWN, HOTKEY_COUNT };
struct KeyBinding { int keys[5]; };
struct GpBinding { int buttons[5]; };
struct HotkeyBinding { KeyBinding kb; GpBinding gp; };

static HotkeyBinding g_hotkeys[HOTKEY_COUNT];
static int g_recordingAction = -1;
static bool g_recordingKeyboard = false;
static bool g_recordingController = false;
static bool g_showHotkeyWindow = false;
static SDL_GameController* g_gameController = nullptr;
static bool g_prevKeyState[256] = {};
static bool g_gpWasPressed = false;
static int g_gpPeakButtons[5] = {-1,-1,-1,-1,-1};

static std::string WideToUtf8(const wchar_t* w) {
    if (!w || !*w) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], len, nullptr, nullptr);
    s.resize(len - 1);
    return s;
}

static std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring wide(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wide[0], len);
    wide.resize(len - 1);
    return wide;
}

static std::string GetExeDir() {
    wchar_t wPath[MAX_PATH];
    GetModuleFileNameW(NULL, wPath, MAX_PATH);
    std::string s = WideToUtf8(wPath);
    return s.substr(0, s.find_last_of("\\/"));
}

static std::string ExtractFilename(const std::string& path) {
    size_t pos = path.find_last_of("\\/");
    return (pos != std::string::npos) ? path.substr(pos + 1) : path;
}

static std::string FormatDuration(float sec) {
    if (sec <= 0.0f) return "";
    int m = (int)sec / 60;
    int s = (int)sec % 60;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return buf;
}

struct TrackMeta { float weight = 1.0f; float volumeMul = 1.0f; };

static std::map<std::string, TrackMeta> ParseTrackMeta(const std::string& dir) {
    std::map<std::string, TrackMeta> meta;
    std::ifstream f(Utf8ToWide(dir) + L"\\tracks.txt");
    if (!f.is_open()) return meta;
    std::string line;
    while (std::getline(f, line)) {
        size_t s = line.find_first_not_of(" \t\r");
        if (s == std::string::npos) continue;
        line = line.substr(s);
        if (line.empty() || line[0] == ';' || line[0] == '/') continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        size_t ne = name.find_last_not_of(" \t");
        if (ne != std::string::npos) name = name.substr(0, ne + 1);
        std::string rest = line.substr(colon + 1);
        for (auto& c : rest) if (c == ',') c = ' ';
        std::stringstream ss(rest);
        TrackMeta tm; float v; int idx = 0;
        while (ss >> v) { if (idx == 0) tm.weight = std::max(0.0f, v); else if (idx == 1) tm.volumeMul = std::max(0.0f, v); idx++; }
        meta[name] = tm;
    }
    return meta;
}

static void BuildShuffleOrder(std::vector<int>& order, const std::vector<CompressedAudio>& pool) {
    order.clear();
    if (pool.empty()) return;
    std::vector<int> rem;
    for (int i = 0; i < (int)pool.size(); i++) {
        if (g_favoritesOnly) {
            std::string name = ExtractFilename(pool[i].filename);
            if (g_favorites.find(name) == g_favorites.end()) continue;
        }
        rem.push_back(i);
    }
    if (rem.empty()) return;
    while (!rem.empty()) {
        float total = 0;
        for (int i : rem) total += std::max(0.0f, pool[i].weight);
        int sel = -1;
        if (total > 0) {
            std::uniform_real_distribution<float> d(0.0f, total);
            float r = d(g_rng), acc = 0;
            for (int i = 0; i < (int)rem.size(); i++) {
                acc += std::max(0.0f, pool[rem[i]].weight);
                if (r < acc) { sel = rem[i]; break; }
            }
        } else {
            sel = rem[std::uniform_int_distribution<int>(0, (int)rem.size() - 1)(g_rng)];
        }
        if (sel >= 0) {
            order.push_back(sel);
            rem.erase(std::remove(rem.begin(), rem.end(), sel), rem.end());
        }
    }
}

static int GetNextIndex(std::vector<CompressedAudio>& pool, std::vector<int>& order, int& pos) {
    if (pool.empty()) return -1;
    if (pos >= (int)order.size()) { BuildShuffleOrder(order, pool); pos = 0; }
    if (order.empty()) return -1;
    return order[pos++];
}

static void LoadMusicFromDir(const std::string& dir, std::vector<CompressedAudio>& pool) {
    auto meta = ParseTrackMeta(dir);
    std::wstring wdir = Utf8ToWide(dir);
    for (const wchar_t* ext : { L"\\*.wav", L"\\*.mp3", L"\\*.ogg", L"\\*.adx", L"\\*.brstm", L"\\*.flac", L"\\*.aac", L"\\*.m4a", L"\\*.aax" }) {
        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW((wdir + ext).c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                std::string name = WideToUtf8(fd.cFileName);
                CompressedAudio ca;
                if (AudioLoader::LoadCompressed(dir + "\\" + name, ca)) {
                    ca.durationSec = AudioLoader::EstimateDuration(ca);
                    auto it = meta.find(name);
                    if (it != meta.end()) { ca.weight = it->second.weight; ca.volumeMul = it->second.volumeMul; }
                    pool.push_back(std::move(ca));
                }
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
    }
}

static void ReloadMusic() {
    g_bgmPool.clear();
    g_lobbyPool.clear();
    g_titlePool.clear();
    std::string dir = GetExeDir();
    LoadMusicFromDir(dir + "\\music", g_bgmPool);
    LoadMusicFromDir(dir + "\\music_lobby", g_lobbyPool);
    LoadMusicFromDir(dir + "\\music_title", g_titlePool);
    BuildShuffleOrder(g_bgmOrder, g_bgmPool); g_bgmOrderPos = 0;
    BuildShuffleOrder(g_lobbyOrder, g_lobbyPool); g_lobbyOrderPos = 0;
    BuildShuffleOrder(g_titleOrder, g_titlePool); g_titleOrderPos = 0;
    if (g_pShared) {
        g_pShared->hasBgmTracks = !g_bgmPool.empty();
        g_pShared->hasLobbyTracks = !g_lobbyPool.empty();
        g_pShared->hasTitleTracks = !g_titlePool.empty();
    }
}

static void LoadFavorites() {
    g_favorites.clear();
    std::ifstream f(Utf8ToWide(GetExeDir()) + L"\\favorites.txt");
    std::string line;
    while (std::getline(f, line)) {
        size_t s = line.find_first_not_of(" \t\r");
        if (s == std::string::npos) continue;
        line = line.substr(s);
        if (!line.empty()) g_favorites.insert(line);
    }
}

static void SaveFavorites() {
    std::ofstream out(Utf8ToWide(GetExeDir()) + L"\\favorites.txt");
    for (auto& f : g_favorites) out << f << "\n";
}

static void LoadRecent() {
    g_recentTracks.clear();
    std::ifstream f(Utf8ToWide(GetExeDir()) + L"\\recent.txt");
    std::string line;
    while (std::getline(f, line)) {
        size_t p = line.find('|');
        if (p != std::string::npos) {
            g_recentTracks.push_back({ line.substr(0, p), std::atoi(line.substr(p + 1).c_str()) });
        }
    }
}

static void SaveRecent() {
    std::ofstream out(Utf8ToWide(GetExeDir()) + L"\\recent.txt");
    for (auto& e : g_recentTracks) out << e.filename << "|" << e.poolIndex << "\n";
}

static void AddRecent(const std::string& name, int pool) {
    for (auto it = g_recentTracks.begin(); it != g_recentTracks.end();) {
        if (it->filename == name && it->poolIndex == pool) it = g_recentTracks.erase(it);
        else ++it;
    }
    g_recentTracks.insert(g_recentTracks.begin(), { name, pool });
    while ((int)g_recentTracks.size() > MAX_RECENT) g_recentTracks.pop_back();
    SaveRecent();
}

static const char* VkToString(int vk) {
    switch (vk) {
    case VK_CONTROL: return "Ctrl";
    case VK_MENU: return "Alt";
    case VK_SHIFT: return "Shift";
    case VK_LEFT: return "Left";
    case VK_RIGHT: return "Right";
    case VK_UP: return "Up";
    case VK_DOWN: return "Down";
    case VK_SPACE: return "Space";
    case VK_RETURN: return "Enter";
    case VK_ESCAPE: return "Esc";
    case VK_TAB: return "Tab";
    case VK_BACK: return "Backspace";
    case VK_DELETE: return "Del";
    case VK_INSERT: return "Ins";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "PageUp";
    case VK_NEXT: return "PageDown";
    default: {
        if (vk >= VK_F1 && vk <= VK_F12) { static char fb[8]; snprintf(fb, sizeof(fb), "F%d", vk - VK_F1 + 1); return fb; }
        if (vk >= '0' && vk <= '9') { static char db[2] = {(char)vk, 0}; return db; }
        if (vk >= 'A' && vk <= 'Z') { static char ab[2] = {(char)vk, 0}; return ab; }
        static char buf[32];
        UINT scan = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
        int len = GetKeyNameTextA(scan << 16, buf, sizeof(buf));
        return (len > 0) ? buf : "?";
    }
    }
}

static const char* GpButtonToString(int btn) {
    switch (btn) {
    case SDL_CONTROLLER_BUTTON_A: return "A";
    case SDL_CONTROLLER_BUTTON_B: return "B";
    case SDL_CONTROLLER_BUTTON_X: return "X";
    case SDL_CONTROLLER_BUTTON_Y: return "Y";
    case SDL_CONTROLLER_BUTTON_BACK: return "Back";
    case SDL_CONTROLLER_BUTTON_GUIDE: return "Guide";
    case SDL_CONTROLLER_BUTTON_START: return "Start";
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "L3";
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "R3";
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "LB";
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "RB";
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return "D-Up";
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "D-Down";
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "D-Left";
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "D-Right";
    case 100: return "L2";
    case 101: return "R2";
    default: return "?";
    }
}

static void FormatKeyBinding(const KeyBinding& kb, char* buf, size_t bufSize) {
    if (kb.keys[0] == -1) { strncpy_s(buf, bufSize, "[None]", _TRUNCATE); return; }
    std::string r;
    for (int i = 0; i < 5 && kb.keys[i] != -1; i++) {
        if (i > 0) r += " + ";
        r += VkToString(kb.keys[i]);
    }
    strncpy_s(buf, bufSize, r.c_str(), _TRUNCATE);
}

static void FormatGpBinding(const GpBinding& gp, char* buf, size_t bufSize) {
    if (gp.buttons[0] == -1) { strncpy_s(buf, bufSize, "[None]", _TRUNCATE); return; }
    std::string r;
    for (int i = 0; i < 5 && gp.buttons[i] != -1; i++) {
        if (i > 0) r += " + ";
        r += GpButtonToString(gp.buttons[i]);
    }
    strncpy_s(buf, bufSize, r.c_str(), _TRUNCATE);
}

static bool IsBindingActive(const HotkeyBinding& hb) {
    if (hb.kb.keys[0] != -1) {
        bool ok = true;
        for (int i = 0; i < 5 && hb.kb.keys[i] != -1; i++)
            if (!(GetAsyncKeyState(hb.kb.keys[i]) & 0x8000)) { ok = false; break; }
        if (ok) return true;
    }
    if (hb.gp.buttons[0] != -1 && g_gameController) {
        bool ok = true;
        for (int i = 0; i < 5 && hb.gp.buttons[i] != -1; i++) {
            int btn = hb.gp.buttons[i];
            bool pressed;
            if (btn == 100)
                pressed = SDL_GameControllerGetAxis(g_gameController, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8192;
            else if (btn == 101)
                pressed = SDL_GameControllerGetAxis(g_gameController, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8192;
            else
                pressed = SDL_GameControllerGetButton(g_gameController, (SDL_GameControllerButton)btn);
            if (!pressed) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

static void SetDefaultHotkeys() {
    memset(g_hotkeys, -1, sizeof(g_hotkeys));
    g_hotkeys[HOTKEY_SKIP].kb.keys[0] = VK_CONTROL;
    g_hotkeys[HOTKEY_SKIP].kb.keys[1] = VK_RIGHT;
    g_hotkeys[HOTKEY_PREV].kb.keys[0] = VK_CONTROL;
    g_hotkeys[HOTKEY_PREV].kb.keys[1] = VK_LEFT;
    g_hotkeys[HOTKEY_VOLUP].kb.keys[0] = VK_CONTROL;
    g_hotkeys[HOTKEY_VOLUP].kb.keys[1] = VK_UP;
    g_hotkeys[HOTKEY_VOLDOWN].kb.keys[0] = VK_CONTROL;
    g_hotkeys[HOTKEY_VOLDOWN].kb.keys[1] = VK_DOWN;
}

static void LoadHotkeys() {
    SetDefaultHotkeys();
    std::ifstream f(Utf8ToWide(GetExeDir()) + L"\\hotkeys.txt");
    if (!f.is_open()) return;
    std::string line;
    auto parseInts = [](const std::string& s, int* out, int maxC) {
        int c = 0;
        std::istringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, ',') && c < maxC) {
            if (tok.empty()) break;
            out[c++] = std::atoi(tok.c_str());
        }
        for (int i = c; i < maxC; i++) out[i] = -1;
    };
    while (std::getline(f, line)) {
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        size_t s = val.find_first_not_of(" \t");
        if (s != std::string::npos) val = val.substr(s);
        int action = -1; bool isKb = false;
        if (key == "Skip_KB") { action = HOTKEY_SKIP; isKb = true; }
        else if (key == "Skip_GP") { action = HOTKEY_SKIP; }
        else if (key == "Prev_KB") { action = HOTKEY_PREV; isKb = true; }
        else if (key == "Prev_GP") { action = HOTKEY_PREV; }
        else if (key == "VolUp_KB") { action = HOTKEY_VOLUP; isKb = true; }
        else if (key == "VolUp_GP") { action = HOTKEY_VOLUP; }
        else if (key == "VolDown_KB") { action = HOTKEY_VOLDOWN; isKb = true; }
        else if (key == "VolDown_GP") { action = HOTKEY_VOLDOWN; }
        if (action >= 0) {
            if (isKb) parseInts(val, g_hotkeys[action].kb.keys, 5);
            else parseInts(val, g_hotkeys[action].gp.buttons, 5);
        }
    }
}

static void SaveHotkeys() {
    std::ofstream out(Utf8ToWide(GetExeDir()) + L"\\hotkeys.txt");
    if (!out.is_open()) return;
    const char* names[] = { "Skip", "Prev", "VolUp", "VolDown" };
    for (int i = 0; i < HOTKEY_COUNT; i++) {
        out << names[i] << "_KB:";
        for (int j = 0; j < 5 && g_hotkeys[i].kb.keys[j] != -1; j++) {
            if (j > 0) out << ",";
            out << g_hotkeys[i].kb.keys[j];
        }
        out << "\n" << names[i] << "_GP:";
        for (int j = 0; j < 5 && g_hotkeys[i].gp.buttons[j] != -1; j++) {
            if (j > 0) out << ",";
            out << g_hotkeys[i].gp.buttons[j];
        }
        out << "\n";
    }
}

static void PollKeyboardRecording() {
    if (!g_recordingKeyboard || g_recordingAction < 0) return;
    for (int k = 8; k < 256; k++) {
        if (k == VK_CONTROL || k == VK_MENU || k == VK_SHIFT) continue;
        if (k == VK_LCONTROL || k == VK_RCONTROL) continue;
        if (k == VK_LMENU || k == VK_RMENU) continue;
        if (k == VK_LSHIFT || k == VK_RSHIFT) continue;
        if (k == VK_LWIN || k == VK_RWIN) continue;
        bool down = (GetAsyncKeyState(k) & 0x8000) != 0;
        if (down && !g_prevKeyState[k]) {
            if (k == VK_ESCAPE) {
                g_hotkeys[g_recordingAction].kb.keys[0] = -1;
                g_recordingKeyboard = false;
                g_recordingAction = -1;
                SaveHotkeys();
            } else {
                KeyBinding& kb = g_hotkeys[g_recordingAction].kb;
                memset(&kb, -1, sizeof(kb));
                int idx = 0;
                if (GetAsyncKeyState(VK_CONTROL) & 0x8000) kb.keys[idx++] = VK_CONTROL;
                if (GetAsyncKeyState(VK_MENU) & 0x8000) kb.keys[idx++] = VK_MENU;
                if (GetAsyncKeyState(VK_SHIFT) & 0x8000) kb.keys[idx++] = VK_SHIFT;
                if (idx < 4) kb.keys[idx++] = k;
                g_recordingKeyboard = false;
                g_recordingAction = -1;
                SaveHotkeys();
            }
            break;
        }
    }
    for (int k = 0; k < 256; k++)
        g_prevKeyState[k] = (GetAsyncKeyState(k) & 0x8000) != 0;
}

static void PollControllerRecording() {
    if (!g_recordingController || g_recordingAction < 0 || !g_gameController) return;
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
        g_hotkeys[g_recordingAction].gp.buttons[0] = -1;
        g_recordingController = false;
        g_recordingAction = -1;
        SaveHotkeys();
        return;
    }
    bool any = false;
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
        if (SDL_GameControllerGetButton(g_gameController, (SDL_GameControllerButton)b)) {
            any = true;
            bool found = false;
            for (int i = 0; i < 5; i++) { if (g_gpPeakButtons[i] == b) { found = true; break; } }
            if (!found) {
                for (int i = 0; i < 4; i++) {
                    if (g_gpPeakButtons[i] == -1) { g_gpPeakButtons[i] = b; break; }
                }
            }
        }
    }
    auto addTrigger = [&](int id) {
        any = true;
        bool found = false;
        for (int i = 0; i < 5; i++) { if (g_gpPeakButtons[i] == id) { found = true; break; } }
        if (!found) {
            for (int i = 0; i < 4; i++) {
                if (g_gpPeakButtons[i] == -1) { g_gpPeakButtons[i] = id; break; }
            }
        }
    };
    if (SDL_GameControllerGetAxis(g_gameController, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8192) addTrigger(100);
    if (SDL_GameControllerGetAxis(g_gameController, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8192) addTrigger(101);
    if (any) {
        g_gpWasPressed = true;
    } else if (g_gpWasPressed) {
        memcpy(g_hotkeys[g_recordingAction].gp.buttons, g_gpPeakButtons, sizeof(g_gpPeakButtons));
        memset(g_gpPeakButtons, -1, sizeof(g_gpPeakButtons));
        g_gpWasPressed = false;
        g_recordingController = false;
        g_recordingAction = -1;
        SaveHotkeys();
    }
}

static void SaveSettings() {
    if (!g_settingsLoaded.load()) return;
    std::ofstream out(GetExeDir() + "\\settings.txt");
    if (out.is_open()) {
        out << "PlayNewMusic: " << (g_playNewMusic ? "true" : "false") << "\n";
        out << "MuteOnUnfocus: " << (g_muteOnUnfocus ? "true" : "false") << "\n";
        out << "Volume: " << g_customBgmVolume.load() << "\n";
        out << "Theme: " << g_themeIndex << "\n";
        out << "MinimizeToTray: " << (g_minimizeToTray ? "true" : "false") << "\n";
        out << "FavoritesOnly: " << (g_favoritesOnly ? "true" : "false") << "\n";
    }
}

static void UpdateSharedStatus(const char* track, const char* poolName, float vol, bool playing) {
    if (!g_pShared) return;
    strncpy_s(g_pShared->currentTrack, track, sizeof(g_pShared->currentTrack) - 1);
    strncpy_s(g_pShared->poolName, poolName, sizeof(g_pShared->poolName) - 1);
    g_pShared->volume = vol;
    g_pShared->isPlaying = playing;
    {
        std::lock_guard<std::mutex> lock(g_trackNameMutex);
        g_currentTrackName = track;
    }
}

static void GetPool(int pid, std::vector<CompressedAudio>*& pool, std::vector<int>*& order, int*& pos, const char*& name) {
    if (pid == 1) { pool = &g_lobbyPool; order = &g_lobbyOrder; pos = &g_lobbyOrderPos; name = "Lobby"; }
    else if (pid == 2) { pool = &g_titlePool; order = &g_titleOrder; pos = &g_titleOrderPos; name = "Title"; }
    else { pool = &g_bgmPool; order = &g_bgmOrder; pos = &g_bgmOrderPos; name = "BGM"; }
}

static void PlayTrack(CompressedAudio& audio, float volume, int categoryId, bool allowLoop) {
    WavData data;
    if (!AudioLoader::DecodeToPcm(audio, data)) return;
    AudioLoader::NormalizePcm16(data);
    AudioLoader::ScalePcm16(data, audio.volumeMul);
    g_audio.PlayPreloaded(data, volume, categoryId, allowLoop);
}

static void DoPlayPause() {
    if (!g_audioInitialized.load()) return;
    if (g_bgmActive.load() && !g_paused.load()) {
        g_audio.PauseCategory(0);
        g_paused.store(true);
    } else if (g_paused.load()) {
        g_audio.ResumeCategory(0);
        g_paused.store(false);
    } else {
        std::vector<CompressedAudio>* pool; std::vector<int>* order; int* pos; const char* pn;
        GetPool(g_activePool, pool, order, pos, pn);
        if (!pool->empty()) {
            int idx = GetNextIndex(*pool, *order, *pos);
            if (idx < 0) return;
            PlayTrack((*pool)[idx], g_customBgmVolume.load(), 0, !g_playNewMusic);
            g_bgmActive.store(true);
            g_playedFinish.store(false);
            g_paused.store(false);
            UpdateSharedStatus(ExtractFilename((*pool)[idx].filename).c_str(), pn, g_customBgmVolume.load(), true);
            AddRecent(ExtractFilename((*pool)[idx].filename), g_activePool);
        }
    }
}

static void DoStop() {
    if (!g_audioInitialized.load()) return;
    g_audio.StopCategoryImmediate(0);
    g_bgmActive.store(false);
    g_paused.store(false);
    UpdateSharedStatus("(none)", "BGM", g_customBgmVolume.load(), false);
}

static void DoSkip() {
    if (!g_audioInitialized.load()) return;
    std::vector<CompressedAudio>* pool; std::vector<int>* order; int* pos; const char* pn;
    GetPool(g_activePool, pool, order, pos, pn);
    if (!pool->empty()) {
        int idx = GetNextIndex(*pool, *order, *pos);
        if (idx < 0) return;
        g_audio.StopCategoryImmediate(0);
        PlayTrack((*pool)[idx], g_customBgmVolume.load(), 0, !g_playNewMusic);
        g_bgmActive.store(true);
        g_paused.store(false);
        g_playedFinish.store(false);
        UpdateSharedStatus(ExtractFilename((*pool)[idx].filename).c_str(), pn, g_customBgmVolume.load(), true);
        AddRecent(ExtractFilename((*pool)[idx].filename), g_activePool);
    }
}

static void DoPrev() {
    if (!g_audioInitialized.load()) return;
    std::vector<CompressedAudio>* pool; std::vector<int>* order; int* pos; const char* pn;
    GetPool(g_activePool, pool, order, pos, pn);
    if (!pool->empty() && *pos > 1) {
        *pos -= 2;
        int idx = GetNextIndex(*pool, *order, *pos);
        if (idx < 0) { DoStop(); return; }
        g_audio.StopCategoryImmediate(0);
        PlayTrack((*pool)[idx], g_customBgmVolume.load(), 0, !g_playNewMusic);
        g_bgmActive.store(true);
        g_paused.store(false);
        g_playedFinish.store(false);
        UpdateSharedStatus(ExtractFilename((*pool)[idx].filename).c_str(), pn, g_customBgmVolume.load(), true);
        AddRecent(ExtractFilename((*pool)[idx].filename), g_activePool);
    } else {
        DoStop();
    }
}

static void OnTrackFinished() {
    if (g_playedFinish.load()) return;
    g_bgmActive.store(false);
    if (!g_playNewMusic) {
        UpdateSharedStatus("(none)", "BGM", g_customBgmVolume.load(), false);
        return;
    }
    std::vector<CompressedAudio>* pool; std::vector<int>* order; int* pos; const char* poolName;
    GetPool(g_activePool, pool, order, pos, poolName);
    if (pool->empty()) return;
    int idx = GetNextIndex(*pool, *order, *pos);
    if (idx < 0) return;
    PlayTrack((*pool)[idx], g_customBgmVolume.load(), 0, false);
    g_bgmActive.store(true);
    std::string name = ExtractFilename((*pool)[idx].filename);
    UpdateSharedStatus(name.c_str(), poolName, g_customBgmVolume.load(), true);
    AddRecent(name, g_activePool);
}

static void AudioThread() {
    while (g_audioThreadRunning) {
        if (g_pShared && g_audioInitialized.load()) {
            g_pShared->hasBgmTracks = !g_bgmPool.empty();
            g_pShared->hasLobbyTracks = !g_lobbyPool.empty();
            g_pShared->hasTitleTracks = !g_titlePool.empty();
        }
        if (g_injected.load() && g_pShared && g_audioInitialized.load()) {
            if (InterlockedCompareExchange(&g_pShared->cmdReady, 0, 1) == 1) {
                int cmd = g_pShared->command;
                int poolId = g_pShared->poolId;
                bool allowLoop = g_pShared->allowLoop;
                float vol = g_customBgmVolume.load();

                if (cmd == 1) {
                    std::vector<CompressedAudio>* pool; std::vector<int>* order; int* pos; const char* poolName;
                    GetPool(poolId, pool, order, pos, poolName);
                    if (!pool->empty()) {
                        if (!(poolId == g_activePool && g_bgmActive.load())) {
                            g_audio.StopCategory(0);
                            g_activePool = poolId;
                            g_playedFinish.store(false);
                            int idx = GetNextIndex(*pool, *order, *pos);
                            if (idx >= 0) {
                                PlayTrack((*pool)[idx], vol, 0, allowLoop);
                                g_bgmActive.store(true);
                                g_paused.store(false);
                                std::string name = ExtractFilename((*pool)[idx].filename);
                                UpdateSharedStatus(name.c_str(), poolName, vol, true);
                                AddRecent(name, poolId);
                            }
                        }
                    }
                } else if (cmd == 3) {
                    g_audio.StopCategory(0);
                    g_bgmActive.store(false);
                    g_playedFinish.store(true);
                    g_paused.store(false);
                    UpdateSharedStatus("(none)", "BGM", vol, false);
                } else if (cmd == 2) {
                    g_audio.StopCategory(0);
                    g_bgmActive.store(false);
                    g_paused.store(false);
                    UpdateSharedStatus("(none)", "BGM", vol, false);
                }
            }
            g_audio.Update();
        }
        Sleep(10);
    }
}

static void InitAudio() {
    std::string dir = GetExeDir();
    LoadMusicFromDir(dir + "\\music", g_bgmPool);
    LoadMusicFromDir(dir + "\\music_lobby", g_lobbyPool);
    LoadMusicFromDir(dir + "\\music_title", g_titlePool);
    LoadFavorites();
    LoadRecent();
    LoadHotkeys();

    std::ifstream settingsFile(dir + "\\settings.txt");
    if (settingsFile.is_open()) {
        std::string line;
        while (std::getline(settingsFile, line)) {
            if (line.find("PlayNewMusic:") != std::string::npos) g_playNewMusic = (line.find("true") != std::string::npos);
            if (line.find("MuteOnUnfocus:") != std::string::npos) g_muteOnUnfocus = (line.find("true") != std::string::npos);
            if (line.find("Volume:") != std::string::npos) {
                size_t c = line.find(':');
                if (c != std::string::npos) {
                    float v = std::strtof(line.substr(c + 1).c_str(), nullptr);
                    if (v >= 0.0f && v <= 5.0f) g_customBgmVolume.store(v);
                }
            }
            if (line.find("Theme:") != std::string::npos) {
                size_t c = line.find(':');
                if (c != std::string::npos) g_themeIndex = std::atoi(line.substr(c + 1).c_str());
            }
            if (line.find("MinimizeToTray:") != std::string::npos) g_minimizeToTray = (line.find("true") != std::string::npos);
            if (line.find("FavoritesOnly:") != std::string::npos) g_favoritesOnly = (line.find("true") != std::string::npos);
        }
    }

    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
        DWORD sz = 0, type;
        g_autostartEnabled = (RegQueryValueExA(hKey, "SonicCustomBGM", NULL, &type, NULL, &sz) == ERROR_SUCCESS && type == REG_SZ);
        RegCloseKey(hKey);
    }

    if (g_audio.Init()) {
        g_audio.OnTrackFinished = OnTrackFinished;
        g_audioInitialized.store(true);
    }
    g_settingsLoaded.store(true);
}

static void SetAutostart(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            char path[MAX_PATH];
            GetModuleFileNameA(NULL, path, MAX_PATH);
            RegSetValueExA(hKey, "SonicCustomBGM", 0, REG_SZ, (BYTE*)path, (DWORD)(strlen(path) + 1));
        } else {
            RegDeleteValueA(hKey, "SonicCustomBGM");
        }
        RegCloseKey(hKey);
    }
    g_autostartEnabled = enable;
}

static HICON CreateTrayIconBitmap() {
    int s = 16;
    HDC hdc = GetDC(NULL);
    HDC memdc = CreateCompatibleDC(hdc);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = s;
    bi.bmiHeader.biHeight = -s;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits;
    HBITMAP hbm = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HBITMAP old = (HBITMAP)SelectObject(memdc, hbm);
    RECT rc = { 0, 0, s, s };
    HBRUSH bg = CreateSolidBrush(RGB(24, 24, 32));
    FillRect(memdc, &rc, bg);
    DeleteObject(bg);
    HBRUSH blue = CreateSolidBrush(RGB(80, 180, 255));
    SelectObject(memdc, blue);
    SelectObject(memdc, GetStockObject(NULL_PEN));
    Ellipse(memdc, 2, 2, s - 2, s - 2);
    DeleteObject(blue);
    SelectObject(memdc, old);
    DeleteDC(memdc);
    ReleaseDC(NULL, hdc);
    ICONINFO ii = {};
    ii.hbmColor = hbm;
    ii.hbmMask = CreateBitmap(s, s, 1, 1, NULL);
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(hbm);
    DeleteObject(ii.hbmMask);
    return icon;
}

static void AddTrayIcon(HWND hWnd) {
    if (g_trayIconVisible) return;
    g_trayIcon = CreateTrayIconBitmap();
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hWnd;
    g_nid.uID = TRAY_ICON_ID;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = g_trayIcon;
    strncpy_s(g_nid.szTip, "Sonic Custom BGM", sizeof(g_nid.szTip));
    Shell_NotifyIconA(NIM_ADD, &g_nid);
    g_trayIconVisible = true;
}

static void RemoveTrayIcon() {
    if (!g_trayIconVisible) return;
    Shell_NotifyIconA(NIM_DELETE, &g_nid);
    if (g_trayIcon) { DestroyIcon(g_trayIcon); g_trayIcon = nullptr; }
    g_trayIconVisible = false;
}

static void ShowTrayMenu(HWND hWnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    AppendMenuA(hMenu, MF_STRING, TRAY_PLAYPAUSE, g_paused.load() ? "Play" : "Pause");
    AppendMenuA(hMenu, MF_STRING, TRAY_SKIP, "Skip");
    AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hMenu, MF_STRING, TRAY_RESTORE, "Show Window");
    AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(hMenu, MF_STRING, TRAY_EXIT, "Exit");
    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
    DestroyMenu(hMenu);
}

static void ShowNowPlayingNotification(const char* trackName) {
    if (!g_trayIconVisible) return;
    g_nid.uFlags |= NIF_INFO;
    strncpy_s(g_nid.szInfoTitle, "Now Playing", sizeof(g_nid.szInfoTitle));
    strncpy_s(g_nid.szInfo, trackName, sizeof(g_nid.szInfo));
    g_nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconA(NIM_MODIFY, &g_nid);
    g_nid.uFlags &= ~NIF_INFO;
}

static void OpenSharedMemory() {
    if (g_hSharedMap && g_pShared) return;
    if (!g_hSharedMap) g_hSharedMap = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, SHARED_MEMORY_NAME);
    if (g_hSharedMap && !g_pShared) g_pShared = (SharedMemory*)MapViewOfFile(g_hSharedMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedMemory));
}

static void CloseSharedMemory() {
    if (g_pShared) { UnmapViewOfFile(g_pShared); g_pShared = nullptr; }
}

static void DoInject(DWORD pid) {
    std::string dllPath = GetExeDir() + "\\SonicCustomBGM.dll";
    if (GetFileAttributesA(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    if (InjectDll(pid, dllPath.c_str())) {
        g_injected.store(true);
        g_targetPid = pid;
        Sleep(500);
        OpenSharedMemory();
        if (g_pShared) {
            g_pShared->hasBgmTracks = !g_bgmPool.empty();
            g_pShared->hasLobbyTracks = !g_lobbyPool.empty();
            g_pShared->hasTitleTracks = !g_titlePool.empty();
        }
    }
    g_connecting = false;
}

static void DoDisconnect() {
    if (g_audioInitialized.load()) g_audio.StopCategoryImmediate(0);
    CloseSharedMemory();
    g_injected.store(false);
    g_targetPid = 0;
    {
        std::lock_guard<std::mutex> lock(g_trackNameMutex);
        g_currentTrackName.clear();
    }
    g_bgmActive.store(false);
    g_paused.store(false);
    SetWindowTextA(g_hWnd, WINDOW_TITLE);
}

static void AutoInjectThread() {
    while (g_scanRunning) {
        if (!g_injected.load() && !g_connecting.load() && g_audioInitialized.load()) {
            auto procs = EnumerateProcesses(L"SonicRacingCrossWorldsSteam.exe");
            if (!procs.empty()) {
                g_connecting = true;
                DoInject(procs[0].pid);
            }
        } else if (g_injected.load() && g_targetPid > 0 && !g_connecting.load()) {
            bool gone = false;
            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, g_targetPid);
            if (hProc) {
                DWORD exitCode;
                GetExitCodeProcess(hProc, &exitCode);
                CloseHandle(hProc);
                gone = (exitCode != STILL_ACTIVE);
            } else {
                gone = (GetLastError() == ERROR_INVALID_PARAMETER);
            }
            if (gone) g_gameGone = true;
        }
        Sleep(2000);
    }
}

static void HandleHotkeys() {
    if (g_recordingKeyboard || g_recordingController) return;

    static bool pU = false, pD = false, pL = false, pR = false;
    bool cU = IsBindingActive(g_hotkeys[HOTKEY_VOLUP]);
    bool cD = IsBindingActive(g_hotkeys[HOTKEY_VOLDOWN]);
    bool cL = IsBindingActive(g_hotkeys[HOTKEY_PREV]);
    bool cR = IsBindingActive(g_hotkeys[HOTKEY_SKIP]);

    static auto lastVolChange = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastVolChange).count();

    if (elapsed >= 100) {
        if (cU && !pU) g_customBgmVolume.store(std::min(5.0f, g_customBgmVolume.load() + 0.1f));
        if (cD && !pD) g_customBgmVolume.store(std::max(0.0f, g_customBgmVolume.load() - 0.1f));
        if ((cU && !pU) || (cD && !pD)) {
            float v = g_customBgmVolume.load();
            if (g_audioInitialized.load()) g_audio.SetCategoryVolume(0, v);
            if (g_pShared) g_pShared->volume = v;
            SaveSettings();
            lastVolChange = now;
        }
    }
    if (cR && !pR && g_audioInitialized.load()) DoSkip();
    if (cL && !pL && g_audioInitialized.load()) DoPrev();
    pU = cU; pD = cD; pL = cL; pR = cR;
}

static bool CreateDeviceD3D(HWND hWnd) {
    g_pD3D = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_pD3D) return false;
    ZeroMemory(&g_d3dpp, sizeof(g_d3dpp));
    g_d3dpp.Windowed = TRUE;
    g_d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_d3dpp.BackBufferFormat = D3DFMT_A8R8G8B8;
    g_d3dpp.BackBufferWidth = WIN_W;
    g_d3dpp.BackBufferHeight = WIN_H;
    g_d3dpp.EnableAutoDepthStencil = TRUE;
    g_d3dpp.AutoDepthStencilFormat = D3DFMT_D16;
    g_d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    HRESULT hr = g_pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &g_d3dpp, &g_pd3dDevice);
    return SUCCEEDED(hr);
}

static void ResetDevice() {
    ImGui_ImplDX9_InvalidateDeviceObjects();
    RECT rc;
    GetClientRect(g_hWnd, &rc);
    g_d3dpp.BackBufferWidth = rc.right;
    g_d3dpp.BackBufferHeight = rc.bottom;
    HRESULT hr = g_pd3dDevice->Reset(&g_d3dpp);
    if (SUCCEEDED(hr))
        ImGui_ImplDX9_CreateDeviceObjects();
}

static void CleanupDevice() {
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
    if (g_pD3D) { g_pD3D->Release(); g_pD3D = nullptr; }
}

static void ApplyTheme(int idx) {
    g_themeIndex = idx;
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* c = style.Colors;
    if (idx == 0) {
        c[ImGuiCol_WindowBg] = ImVec4(0.094f, 0.094f, 0.125f, 1.0f);
        c[ImGuiCol_ChildBg] = ImVec4(0.125f, 0.125f, 0.173f, 1.0f);
        c[ImGuiCol_PopupBg] = ImVec4(0.118f, 0.118f, 0.157f, 0.96f);
        c[ImGuiCol_Border] = ImVec4(0.235f, 0.235f, 0.314f, 0.5f);
        c[ImGuiCol_FrameBg] = ImVec4(0.160f, 0.160f, 0.220f, 1.0f);
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.240f, 0.240f, 0.320f, 1.0f);
        c[ImGuiCol_FrameBgActive] = ImVec4(0.120f, 0.120f, 0.180f, 1.0f);
        c[ImGuiCol_TitleBg] = ImVec4(0.094f, 0.094f, 0.125f, 1.0f);
        c[ImGuiCol_TitleBgActive] = ImVec4(0.094f, 0.094f, 0.125f, 1.0f);
        c[ImGuiCol_ScrollbarBg] = ImVec4(0.094f, 0.094f, 0.125f, 0.5f);
        c[ImGuiCol_ScrollbarGrab] = ImVec4(0.200f, 0.200f, 0.280f, 1.0f);
        c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.280f, 0.280f, 0.380f, 1.0f);
        c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.160f, 0.160f, 0.220f, 1.0f);
        c[ImGuiCol_CheckMark] = ImVec4(0.863f, 0.863f, 0.902f, 1.0f);
        c[ImGuiCol_SliderGrab] = ImVec4(0.314f, 0.706f, 1.0f, 1.0f);
        c[ImGuiCol_SliderGrabActive] = ImVec4(0.400f, 0.800f, 1.0f, 1.0f);
        c[ImGuiCol_Button] = ImVec4(0.196f, 0.196f, 0.267f, 1.0f);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.275f, 0.275f, 0.376f, 1.0f);
        c[ImGuiCol_ButtonActive] = ImVec4(0.157f, 0.157f, 0.220f, 1.0f);
        c[ImGuiCol_Header] = ImVec4(0.196f, 0.196f, 0.267f, 1.0f);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.275f, 0.275f, 0.376f, 1.0f);
        c[ImGuiCol_HeaderActive] = ImVec4(0.157f, 0.157f, 0.220f, 1.0f);
        c[ImGuiCol_Tab] = ImVec4(0.125f, 0.125f, 0.173f, 1.0f);
        c[ImGuiCol_TabHovered] = ImVec4(0.275f, 0.275f, 0.376f, 1.0f);
        c[ImGuiCol_TabActive] = ImVec4(0.196f, 0.196f, 0.267f, 1.0f);
        c[ImGuiCol_TabUnfocused] = ImVec4(0.125f, 0.125f, 0.173f, 1.0f);
        c[ImGuiCol_TabUnfocusedActive] = ImVec4(0.196f, 0.196f, 0.267f, 1.0f);
        c[ImGuiCol_Separator] = ImVec4(0.235f, 0.235f, 0.314f, 0.5f);
        c[ImGuiCol_Text] = ImVec4(0.863f, 0.863f, 0.902f, 1.0f);
        c[ImGuiCol_TextDisabled] = ImVec4(0.510f, 0.510f, 0.588f, 1.0f);
    } else if (idx == 1) {
        c[ImGuiCol_WindowBg] = ImVec4(0.92f, 0.92f, 0.94f, 1.0f);
        c[ImGuiCol_ChildBg] = ImVec4(0.85f, 0.85f, 0.88f, 1.0f);
        c[ImGuiCol_PopupBg] = ImVec4(0.95f, 0.95f, 0.97f, 0.96f);
        c[ImGuiCol_Border] = ImVec4(0.70f, 0.70f, 0.75f, 0.5f);
        c[ImGuiCol_FrameBg] = ImVec4(0.80f, 0.80f, 0.84f, 1.0f);
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.72f, 0.72f, 0.78f, 1.0f);
        c[ImGuiCol_FrameBgActive] = ImVec4(0.88f, 0.88f, 0.92f, 1.0f);
        c[ImGuiCol_TitleBg] = ImVec4(0.82f, 0.82f, 0.86f, 1.0f);
        c[ImGuiCol_TitleBgActive] = ImVec4(0.75f, 0.75f, 0.80f, 1.0f);
        c[ImGuiCol_ScrollbarBg] = ImVec4(0.88f, 0.88f, 0.92f, 0.5f);
        c[ImGuiCol_ScrollbarGrab] = ImVec4(0.65f, 0.65f, 0.72f, 1.0f);
        c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.55f, 0.55f, 0.62f, 1.0f);
        c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.50f, 0.50f, 0.58f, 1.0f);
        c[ImGuiCol_CheckMark] = ImVec4(0.15f, 0.45f, 0.75f, 1.0f);
        c[ImGuiCol_SliderGrab] = ImVec4(0.20f, 0.50f, 0.85f, 1.0f);
        c[ImGuiCol_SliderGrabActive] = ImVec4(0.15f, 0.40f, 0.75f, 1.0f);
        c[ImGuiCol_Button] = ImVec4(0.78f, 0.78f, 0.83f, 1.0f);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.70f, 0.70f, 0.77f, 1.0f);
        c[ImGuiCol_ButtonActive] = ImVec4(0.85f, 0.85f, 0.90f, 1.0f);
        c[ImGuiCol_Header] = ImVec4(0.72f, 0.72f, 0.78f, 1.0f);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.65f, 0.65f, 0.72f, 1.0f);
        c[ImGuiCol_HeaderActive] = ImVec4(0.80f, 0.80f, 0.86f, 1.0f);
        c[ImGuiCol_Tab] = ImVec4(0.82f, 0.82f, 0.86f, 1.0f);
        c[ImGuiCol_TabHovered] = ImVec4(0.65f, 0.65f, 0.72f, 1.0f);
        c[ImGuiCol_TabActive] = ImVec4(0.72f, 0.72f, 0.78f, 1.0f);
        c[ImGuiCol_TabUnfocused] = ImVec4(0.85f, 0.85f, 0.88f, 1.0f);
        c[ImGuiCol_TabUnfocusedActive] = ImVec4(0.78f, 0.78f, 0.83f, 1.0f);
        c[ImGuiCol_Separator] = ImVec4(0.70f, 0.70f, 0.75f, 0.5f);
        c[ImGuiCol_Text] = ImVec4(0.10f, 0.10f, 0.13f, 1.0f);
        c[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.55f, 1.0f);
    } else {
        c[ImGuiCol_WindowBg] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
        c[ImGuiCol_ChildBg] = ImVec4(0.05f, 0.05f, 0.05f, 1.0f);
        c[ImGuiCol_PopupBg] = ImVec4(0.05f, 0.05f, 0.05f, 0.96f);
        c[ImGuiCol_Border] = ImVec4(1.0f, 1.0f, 1.0f, 0.3f);
        c[ImGuiCol_FrameBg] = ImVec4(0.10f, 0.10f, 0.10f, 1.0f);
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.20f, 0.20f, 1.0f);
        c[ImGuiCol_FrameBgActive] = ImVec4(0.05f, 0.05f, 0.05f, 1.0f);
        c[ImGuiCol_TitleBg] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
        c[ImGuiCol_TitleBgActive] = ImVec4(0.15f, 0.15f, 0.15f, 1.0f);
        c[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.5f);
        c[ImGuiCol_ScrollbarGrab] = ImVec4(0.40f, 0.40f, 0.40f, 1.0f);
        c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.60f, 0.60f, 0.60f, 1.0f);
        c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.30f, 0.30f, 0.30f, 1.0f);
        c[ImGuiCol_CheckMark] = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
        c[ImGuiCol_SliderGrab] = ImVec4(0.0f, 0.8f, 0.0f, 1.0f);
        c[ImGuiCol_SliderGrabActive] = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
        c[ImGuiCol_Button] = ImVec4(0.15f, 0.15f, 0.15f, 1.0f);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.25f, 0.25f, 0.25f, 1.0f);
        c[ImGuiCol_ButtonActive] = ImVec4(0.10f, 0.10f, 0.10f, 1.0f);
        c[ImGuiCol_Header] = ImVec4(0.15f, 0.15f, 0.15f, 1.0f);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.25f, 0.25f, 0.25f, 1.0f);
        c[ImGuiCol_HeaderActive] = ImVec4(0.10f, 0.10f, 0.10f, 1.0f);
        c[ImGuiCol_Tab] = ImVec4(0.05f, 0.05f, 0.05f, 1.0f);
        c[ImGuiCol_TabHovered] = ImVec4(0.25f, 0.25f, 0.25f, 1.0f);
        c[ImGuiCol_TabActive] = ImVec4(0.15f, 0.15f, 0.15f, 1.0f);
        c[ImGuiCol_TabUnfocused] = ImVec4(0.05f, 0.05f, 0.05f, 1.0f);
        c[ImGuiCol_TabUnfocusedActive] = ImVec4(0.15f, 0.15f, 0.15f, 1.0f);
        c[ImGuiCol_Separator] = ImVec4(1.0f, 1.0f, 1.0f, 0.2f);
        c[ImGuiCol_Text] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        c[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.50f, 1.0f);
    }
    SaveSettings();
}

static bool PassesFilter(const std::string& name) {
    if (g_favoritesOnly && g_favorites.find(name) == g_favorites.end()) return false;
    if (g_searchBuf[0] != '\0') {
        std::string lower = name;
        std::string filter = g_searchBuf;
        for (auto& c : lower) c = (char)tolower(c);
        for (auto& c : filter) c = (char)tolower(c);
        if (lower.find(filter) == std::string::npos) return false;
    }
    return true;
}

static void RenderHotkeyWindow() {
    if (!g_showHotkeyWindow) return;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2((ds.x - 520) * 0.5f, (ds.y - 280) * 0.5f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(520, 280), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Configure Hotkeys", &g_showHotkeyWindow)) { ImGui::End(); return; }

    if (g_recordingKeyboard || g_recordingController) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.0f, 1.0f),
            g_recordingKeyboard ? "Press a key combination..." : "Press controller buttons...");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) { g_recordingKeyboard = g_recordingController = false; g_recordingAction = -1; }
        ImGui::Spacing();
    }

    struct ActDef { const char* label; int action; };
    ActDef acts[] = { {"Skip Track", HOTKEY_SKIP}, {"Previous Track", HOTKEY_PREV}, {"Volume Up", HOTKEY_VOLUP}, {"Volume Down", HOTKEY_VOLDOWN} };

    if (ImGui::BeginTable("##hk", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Action", 0, 0.35f);
        ImGui::TableSetupColumn("Keyboard", 0, 0.325f);
        ImGui::TableSetupColumn("Controller", 0, 0.325f);
        ImGui::TableHeadersRow();
        for (auto& a : acts) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(a.label);

            ImGui::TableSetColumnIndex(1);
            char kbBuf[64];
            FormatKeyBinding(g_hotkeys[a.action].kb, kbBuf, sizeof(kbBuf));
            if (g_recordingKeyboard && g_recordingAction == a.action) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.6f, 0.0f, 1.0f));
                if (ImGui::SmallButton("Listening...")) { g_recordingKeyboard = false; g_recordingAction = -1; }
                ImGui::PopStyleColor();
            } else {
                if (ImGui::SmallButton(kbBuf)) { g_recordingKeyboard = true; g_recordingController = false; g_recordingAction = a.action; }
            }

            ImGui::TableSetColumnIndex(2);
            char gpBuf[64];
            FormatGpBinding(g_hotkeys[a.action].gp, gpBuf, sizeof(gpBuf));
            if (g_recordingController && g_recordingAction == a.action) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.6f, 0.0f, 1.0f));
                if (ImGui::SmallButton("Listening...")) { g_recordingController = false; g_recordingAction = -1; }
                ImGui::PopStyleColor();
            } else {
                if (ImGui::SmallButton(gpBuf)) { g_recordingController = true; g_recordingKeyboard = false; g_recordingAction = a.action; memset(g_gpPeakButtons, -1, sizeof(g_gpPeakButtons)); }
            }
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Reset to Defaults")) { SetDefaultHotkeys(); SaveHotkeys(); }
    ImGui::SameLine();
    if (ImGui::Button("Close")) g_showHotkeyWindow = false;

    ImGui::End();
}

static void RenderUI() {
    ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(displaySize);
    ImGui::Begin("##Main", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);

    bool inj = g_injected.load();
    bool conn = g_connecting.load();
    SharedMemory snap = {};
    if (g_pShared) memcpy(&snap, g_pShared, sizeof(snap));

    float contentW = displaySize.x - 24.0f;

    ImGui::PushFont(g_fontBold);
    if (!inj && !conn) {
        ImGui::TextColored(ImVec4(0.94f, 0.78f, 0.24f, 1.0f), "Scanning for SonicRacingCrossWorldsSteam.exe ...");
    } else if (conn) {
        ImGui::TextColored(ImVec4(0.94f, 0.78f, 0.24f, 1.0f), "Connecting ...");
    } else if (snap.active) {
        ImGui::TextColored(ImVec4(0.31f, 0.78f, 0.47f, 1.0f), "Connected to PID %lu", g_targetPid);
    } else {
        ImGui::TextColored(ImVec4(0.94f, 0.78f, 0.24f, 1.0f), "Connected to PID %lu (waiting ...)", g_targetPid);
    }
    ImGui::PopFont();

    ImGui::SameLine(contentW - 60.0f);
    {
        ImVec2 gp = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##credits", ImVec2(22, 22));
        if (ImGui::IsItemClicked()) g_showCreditsPopup = true;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float cx = gp.x + 11, cy = gp.y + 11;
        ImU32 col = ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        dl->AddCircleFilled(ImVec2(cx, cy), 9.0f, col);
        dl->AddText(ImVec2(cx - 3.5f, cy - 7.0f), ImGui::GetColorU32(ImGui::GetStyleColorVec4(ImGuiCol_WindowBg)), "i");
    }

    ImGui::SameLine(contentW - 30.0f);
    {
        ImVec2 gp = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##settings", ImVec2(22, 22));
        if (ImGui::IsItemClicked()) g_showSettingsPopup = !g_showSettingsPopup;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float cx = gp.x + 11, cy = gp.y + 11;
        ImU32 col = ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        float outerR = 8.0f, innerR = 4.5f, toothH = 2.5f, toothW = 2.0f;
        int teeth = 8;
        for (int i = 0; i < teeth; i++) {
            float a0 = (float)i / teeth * 6.283185f;
            float a1 = a0 + (1.5f / teeth) * 6.283185f * 0.5f;
            float cos0 = cosf(a0), sin0 = sinf(a0);
            float cos1 = cosf(a1), sin1 = sinf(a1);
            ImVec2 p0(cx + cos0 * outerR, cy + sin0 * outerR);
            ImVec2 p1(cx + cos1 * (outerR + toothH), cy + sin1 * (outerR + toothH));
            ImVec2 p2(cx + cosf(a0 + 3.14159f / teeth) * (outerR + toothH), cy + sinf(a0 + 3.14159f / teeth) * (outerR + toothH));
            ImVec2 p3(cx + cosf(a1 + 3.14159f / teeth * 2) * outerR, cy + sinf(a1 + 3.14159f / teeth * 2) * outerR);
            dl->AddQuadFilled(p0, p1, p2, p3, col);
        }
        dl->AddCircleFilled(ImVec2(cx, cy), outerR, ImGui::GetColorU32(ImGui::GetStyleColorVec4(ImGuiCol_WindowBg)));
        dl->AddCircleFilled(ImVec2(cx, cy), innerR, col);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextDisabled("NOW PLAYING");
    ImGui::PushFont(g_fontBold);
    const char* trackName = (inj && snap.currentTrack[0]) ? snap.currentTrack : "(none)";
    ImGui::TextColored(ImVec4(0.31f, 0.71f, 1.0f, 1.0f), "%s", trackName);
    ImGui::PopFont();

    ImGui::TextDisabled("Pool: %s", (inj && snap.poolName[0]) ? snap.poolName : "--");

    ImGui::Spacing();

    float btnW = 38.0f;
    float btnH = 38.0f;
    float totalBtnW = btnW * 4 + ImGui::GetStyle().ItemSpacing.x * 3;
    float btnX = (displaySize.x - totalBtnW) / 2.0f;
    ImGui::SetCursorPosX(btnX);

    ImVec4 activeCol = (g_themeIndex == 1) ? ImVec4(0.15f, 0.45f, 0.75f, 1.0f) :
                       (g_themeIndex == 2) ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f) :
                       ImVec4(0.31f, 0.71f, 1.0f, 1.0f);
    ImU32 iCol = ImGui::GetColorU32(activeCol);

    auto IconBtn = [](const char* id, ImVec2 size) -> bool {
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(id, size);
        ImU32 bg = ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_ButtonActive : (ImGui::IsItemHovered() ? ImGuiCol_ButtonHovered : ImGuiCol_Button));
        ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bg, 4.0f);
        return ImGui::IsItemClicked();
    };

    if (IconBtn("##prev", ImVec2(btnW, btnH))) DoPrev();
    {
        ImVec2 p = ImGui::GetItemRectMin();
        float cx = p.x + btnW * 0.5f, cy = p.y + btnH * 0.5f;
        float r = btnH * 0.28f;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(cx - r * 1.1f, cy - r), ImVec2(cx - r * 0.85f, cy + r), iCol);
        ImGui::GetWindowDrawList()->AddTriangleFilled(ImVec2(cx + r * 0.8f, cy - r), ImVec2(cx + r * 0.8f, cy + r), ImVec2(cx - r * 0.7f, cy), iCol);
    }

    ImGui::SameLine();
    {
        const char* playId = (g_paused.load() || !g_bgmActive.load()) ? "##play" : "##pause";
        if (IconBtn(playId, ImVec2(btnW, btnH))) DoPlayPause();
        ImVec2 p = ImGui::GetItemRectMin();
        float cx = p.x + btnW * 0.5f, cy = p.y + btnH * 0.5f;
        float r = btnH * 0.32f;
        if (g_paused.load() || !g_bgmActive.load()) {
            ImGui::GetWindowDrawList()->AddTriangleFilled(ImVec2(cx - r * 0.7f, cy - r), ImVec2(cx - r * 0.7f, cy + r), ImVec2(cx + r * 0.9f, cy), iCol);
        } else {
            float bw = r * 0.35f;
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(cx - r * 0.4f - bw, cy - r), ImVec2(cx - r * 0.4f, cy + r), iCol);
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(cx + r * 0.4f - bw, cy - r), ImVec2(cx + r * 0.4f, cy + r), iCol);
        }
    }

    ImGui::SameLine();
    if (IconBtn("##stop", ImVec2(btnW, btnH))) DoStop();
    {
        ImVec2 p = ImGui::GetItemRectMin();
        float cx = p.x + btnW * 0.5f, cy = p.y + btnH * 0.5f;
        float r = btnH * 0.28f;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(cx - r, cy - r), ImVec2(cx + r, cy + r), iCol);
    }

    ImGui::SameLine();
    if (IconBtn("##next", ImVec2(btnW, btnH))) DoSkip();
    {
        ImVec2 p = ImGui::GetItemRectMin();
        float cx = p.x + btnW * 0.5f, cy = p.y + btnH * 0.5f;
        float r = btnH * 0.28f;
        ImGui::GetWindowDrawList()->AddTriangleFilled(ImVec2(cx - r * 0.8f, cy - r), ImVec2(cx - r * 0.8f, cy + r), ImVec2(cx + r * 0.7f, cy), iCol);
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(cx + r * 0.85f, cy - r), ImVec2(cx + r * 1.1f, cy + r), iCol);
    }

    ImGui::Spacing();

    float vol = g_customBgmVolume.load();
    float volPct = vol * 100.0f;
    if (ImGui::SliderFloat("##vol", &volPct, 0.0f, 500.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
        vol = volPct / 100.0f;
        g_customBgmVolume.store(vol);
        if (g_audioInitialized.load()) g_audio.SetCategoryVolume(0, vol);
        if (g_pShared) g_pShared->volume = vol;
        SaveSettings();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushItemWidth(contentW - 36.0f);
    ImGui::InputTextWithHint("##search", "Search tracks...", g_searchBuf, sizeof(g_searchBuf));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    {
        ImVec2 rp = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##refresh", ImVec2(28, ImGui::GetFrameHeight()));
        bool hovered = ImGui::IsItemHovered();
        bool clicked = ImGui::IsItemClicked();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float cx = rp.x + 14, cy = rp.y + ImGui::GetFrameHeight() * 0.5f;
        ImU32 col = ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        float r = 7.0f;
        dl->AddCircle(ImVec2(cx, cy), r, col, 0, 1.5f);
        int segs = 12;
        for (int i = 0; i < segs; i++) {
            float a0 = (float)i / segs * 6.283185f - 1.2f;
            float a1 = a0 + 0.35f;
            dl->AddLine(
                ImVec2(cx + cosf(a0) * (r - 1), cy + sinf(a0) * (r - 1)),
                ImVec2(cx + cosf(a1) * (r - 1), cy + sinf(a1) * (r - 1)), col, 2.0f);
        }
        ImVec2 tip = ImVec2(cx + cosf(-1.2f) * (r + 2), cy + sinf(-1.2f) * (r + 2));
        dl->AddTriangleFilled(tip,
            ImVec2(tip.x - 4, tip.y + 1), ImVec2(tip.x + 1, tip.y + 5), col);
        if (clicked) ReloadMusic();
    }

    ImGui::Spacing();

    static int currentTab = 0;
    const char* tabNames[] = { "BGM", "Lobby", "Title", "Recent" };
    int counts[] = { (int)g_bgmPool.size(), (int)g_lobbyPool.size(), (int)g_titlePool.size(), (int)g_recentTracks.size() };

    if (ImGui::BeginTabBar("##Tabs", ImGuiTabBarFlags_NoCloseWithMiddleMouseButton)) {
        for (int i = 0; i < 4; i++) {
            char label[64];
            snprintf(label, sizeof(label), "%s (%d)", tabNames[i], counts[i]);
            if (ImGui::BeginTabItem(label)) {
                currentTab = i;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    std::string curTrack;
    {
        std::lock_guard<std::mutex> lock(g_trackNameMutex);
        curTrack = g_currentTrackName;
    }

    float trackListH = ImGui::GetContentRegionAvail().y - 110.0f;
    if (trackListH < 100.0f) trackListH = 100.0f;
    ImGui::BeginChild("##TrackList", ImVec2(0, trackListH), true);

    std::string lastNotified;
    {
        std::lock_guard<std::mutex> lock(g_trackNameMutex);
        lastNotified = g_lastNotifiedTrack;
    }

    if (currentTab == 3) {
        if (g_recentTracks.empty()) {
            ImGui::TextDisabled("No tracks played yet");
        } else {
            for (int i = 0; i < (int)g_recentTracks.size(); i++) {
                auto& entry = g_recentTracks[i];
                bool isPlaying = g_bgmActive.load() && curTrack == entry.filename;
                const char* poolLabel = entry.poolIndex == 1 ? "[Lobby] " : entry.poolIndex == 2 ? "[Title] " : "[BGM] ";
                char label[512];
                snprintf(label, sizeof(label), "%s%s%s", isPlaying ? "> " : "  ", poolLabel, entry.filename.c_str());
                if (isPlaying) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.31f, 0.71f, 1.0f, 1.0f));
                if (ImGui::Selectable(label, false, ImGuiSelectableFlags_AllowDoubleClick)) {
                    if (g_audioInitialized.load()) {
                        std::vector<CompressedAudio>* pool; std::vector<int>* order; int* pos; const char* pn;
                        GetPool(entry.poolIndex, pool, order, pos, pn);
                        for (int j = 0; j < (int)pool->size(); j++) {
                            if (ExtractFilename((*pool)[j].filename) == entry.filename) {
                                g_audio.StopCategoryImmediate(0);
                                PlayTrack((*pool)[j], g_customBgmVolume.load(), 0, !g_playNewMusic);
                                g_bgmActive.store(true);
                                g_paused.store(false);
                                g_playedFinish.store(false);
                                g_activePool = entry.poolIndex;
                                UpdateSharedStatus(entry.filename.c_str(), pn, g_customBgmVolume.load(), true);
                                break;
                            }
                        }
                    }
                }
                if (isPlaying) ImGui::PopStyleColor();
            }
        }
    } else {
        std::vector<CompressedAudio>* pools[] = { &g_bgmPool, &g_lobbyPool, &g_titlePool };
        std::vector<CompressedAudio>* pool = pools[currentTab];
        if (pool->empty()) {
            ImGui::TextDisabled("No tracks loaded");
        } else {
            for (int i = 0; i < (int)pool->size(); i++) {
                std::string name = ExtractFilename((*pool)[i].filename);
                if (!PassesFilter(name)) continue;
                bool isPlaying = g_bgmActive.load() && curTrack == name;
                bool isFav = g_favorites.count(name) > 0;

                ImVec2 btnPos = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton(("##f" + std::to_string(currentTab) + "_" + std::to_string(i)).c_str(), ImVec2(14, ImGui::GetFrameHeight()));
                if (ImGui::IsItemClicked()) {
                    if (isFav) g_favorites.erase(name); else g_favorites.insert(name);
                    SaveFavorites();
                }
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 center = ImVec2(btnPos.x + 7, btnPos.y + ImGui::GetFrameHeight() * 0.5f);
                if (isFav)
                    dl->AddCircleFilled(center, 5.0f, ImGui::GetColorU32(ImVec4(1.0f, 0.85f, 0.0f, 1.0f)));
                else
                    dl->AddCircle(center, 5.0f, ImGui::GetColorU32(ImVec4(0.35f, 0.35f, 0.40f, 0.5f)));
                ImGui::SameLine();

                std::string dur = FormatDuration((*pool)[i].durationSec);
                char label[512];
                if (!dur.empty())
                    snprintf(label, sizeof(label), "%s%-40s %s", isPlaying ? "> " : "  ", name.c_str(), dur.c_str());
                else
                    snprintf(label, sizeof(label), "%s%s", isPlaying ? "> " : "  ", name.c_str());

                if (isPlaying) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.31f, 0.71f, 1.0f, 1.0f));

                if (ImGui::Selectable(label, false, ImGuiSelectableFlags_AllowDoubleClick)) {
                    if (g_audioInitialized.load()) {
                        g_audio.StopCategoryImmediate(0);
                        PlayTrack((*pool)[i], g_customBgmVolume.load(), 0, !g_playNewMusic);
                        g_bgmActive.store(true);
                        g_paused.store(false);
                        g_playedFinish.store(false);
                        g_activePool = currentTab;
                        UpdateSharedStatus(name.c_str(), tabNames[currentTab], g_customBgmVolume.load(), true);
                        AddRecent(name, currentTab);
                    }
                }

                if (isPlaying) ImGui::PopStyleColor();
            }
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushFont(g_fontMono);
    {
        char buf[64];
        FormatKeyBinding(g_hotkeys[HOTKEY_VOLUP].kb, buf, sizeof(buf));
        ImGui::TextDisabled("  %s Volume", buf);
    }
    ImGui::SameLine(displaySize.x * 0.37f);
    {
        char buf[64];
        FormatKeyBinding(g_hotkeys[HOTKEY_SKIP].kb, buf, sizeof(buf));
        ImGui::TextDisabled("  %s     Skip", buf);
    }
    ImGui::SameLine(displaySize.x * 0.67f);
    {
        char buf[64];
        FormatKeyBinding(g_hotkeys[HOTKEY_PREV].kb, buf, sizeof(buf));
        ImGui::TextDisabled("  %s      Prev", buf);
    }
    ImGui::PopFont();

    ImGui::End();

    if (g_lastNotifiedTrack.empty() || g_lastNotifiedTrack != curTrack) {
        if (!curTrack.empty() && curTrack != "(none)" && g_audioInitialized.load()) {
            ShowNowPlayingNotification(curTrack.c_str());
            g_lastNotifiedTrack = curTrack;
        }
    }

    if (g_showSettingsPopup) {
        ImGui::OpenPopup("Settings");
        g_showSettingsPopup = false;
    }

    if (ImGui::BeginPopupModal("Settings", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Playback");
        ImGui::Indent();
        if (ImGui::Checkbox("Shuffle (play next after song ends)", &g_playNewMusic)) SaveSettings();
        if (ImGui::Checkbox("Mute when window loses focus", &g_muteOnUnfocus)) SaveSettings();
        if (ImGui::Checkbox("Favorites only", &g_favoritesOnly)) SaveSettings();
        ImGui::Unindent();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Appearance");
        ImGui::Indent();
        const char* themes[] = { "Dark", "Light", "High Contrast" };
        if (ImGui::Combo("Theme", &g_themeIndex, themes, 3)) ApplyTheme(g_themeIndex);
        ImGui::Unindent();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("System");
        ImGui::Indent();
        if (ImGui::Checkbox("Minimize to system tray", &g_minimizeToTray)) SaveSettings();
        if (ImGui::Checkbox("Start with Windows", &g_autostartEnabled)) SetAutostart(g_autostartEnabled);
        ImGui::Unindent();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Controls");
        ImGui::Indent();
        if (ImGui::Button("Configure Hotkeys...")) { g_showHotkeyWindow = true; ImGui::CloseCurrentPopup(); }
        ImGui::Unindent();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Favorites: %d tracks", (int)g_favorites.size());
        ImGui::TextDisabled("Click the circle next to a track to toggle favorite");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Close", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (g_showProcessPopup) {
        g_processList = EnumerateProcesses(L"SonicRacingCrossWorldsSteam.exe");
        ImGui::OpenPopup("Select Process");
        g_showProcessPopup = false;
    }

    if (ImGui::BeginPopupModal("Select Process", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (g_processList.empty()) {
            ImGui::Text("No processes found.");
        } else {
            ImGui::Text("Select a process to inject into:");
            ImGui::Spacing();
            for (size_t i = 0; i < g_processList.size(); i++) {
                char label[256];
                snprintf(label, sizeof(label), "PID %lu - %ls", g_processList[i].pid, g_processList[i].name.c_str());
                if (ImGui::Selectable(label)) {
                    DoInject(g_processList[i].pid);
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::Separator();
        if (ImGui::Button("Refresh")) {
            g_processList = EnumerateProcesses(L"SonicRacingCrossWorldsSteam.exe");
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (g_showCreditsPopup) {
        ImGui::OpenPopup("About");
        g_showCreditsPopup = false;
    }

    if (ImGui::BeginPopupModal("About", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushFont(g_fontBold);
        ImGui::Text("Sonic Custom BGM");
        ImGui::PopFont();
        ImGui::TextDisabled("Replace game BGM with your own music");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::PushFont(g_fontBold);
        ImGui::Text("Developer");
        ImGui::PopFont();
        ImGui::TextDisabled("  RED1");
        ImGui::Spacing();

        ImGui::PushFont(g_fontBold);
        ImGui::Text("Special Thanks");
        ImGui::PopFont();
        ImGui::TextDisabled("  RyoTune - CRI Atom / ACB information (Ryo Framework)");
        ImGui::Spacing();

        ImGui::PushFont(g_fontBold);
        ImGui::Text("Libraries");
        ImGui::PopFont();
        ImGui::TextDisabled("  Dear ImGui          - Omar Cornut");
        ImGui::TextDisabled("  MinHook             - Tsuda Kagewo");
        ImGui::TextDisabled("  SDL2                - Sam Lantinga");
        ImGui::TextDisabled("  dr_mp3              - David Reid");
        ImGui::TextDisabled("  dr_flac             - David Reid");
        ImGui::TextDisabled("  stb_vorbis          - Sean Barrett");
        ImGui::TextDisabled("  libhelix-aac        - Ahead Software / RealNetworks");
        ImGui::TextDisabled("  XAudio2             - Microsoft");
        ImGui::TextDisabled("  Direct3D 9          - Microsoft");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    RenderHotkeyWindow();
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED && g_minimizeToTray) {
            AddTrayIcon(hWnd);
            ShowWindow(hWnd, SW_HIDE);
            return 0;
        }
        if (g_pd3dDevice && wParam != SIZE_MINIMIZED) {
            ResetDevice();
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TRAYICON:
        if (lParam == WM_LBUTTONDBLCLK) {
            ShowWindow(hWnd, SW_RESTORE);
            RemoveTrayIcon();
            SetForegroundWindow(hWnd);
        } else if (lParam == WM_RBUTTONUP) {
            ShowTrayMenu(hWnd);
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case TRAY_RESTORE:
            ShowWindow(hWnd, SW_RESTORE);
            RemoveTrayIcon();
            SetForegroundWindow(hWnd);
            break;
        case TRAY_PLAYPAUSE:
            DoPlayPause();
            break;
        case TRAY_SKIP:
            DoSkip();
            break;
        case TRAY_EXIT:
            RemoveTrayIcon();
            g_scanRunning = false;
            g_audioThreadRunning = false;
            DestroyWindow(hWnd);
            break;
        }
        return 0;
    case WM_DESTROY:
        RemoveTrayIcon();
        g_scanRunning = false;
        g_audioThreadRunning = false;
        CloseSharedMemory();
        if (g_hSharedMap) { CloseHandle(g_hSharedMap); g_hSharedMap = nullptr; }
        CleanupDevice();
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        if (g_gameController) { SDL_GameControllerClose(g_gameController); g_gameController = nullptr; }
        SDL_Quit();
        PostQuitMessage(0);
        return 0;
    case WM_APP_GONE:
        DoDisconnect();
        g_gameGone = false;
        return 0;
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    SetProcessDPIAware();

    HANDLE hMutex = CreateMutexA(NULL, FALSE, "SonicCustomBGM_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowA("SonicCustomBGM_ImGui", WINDOW_TITLE);
        if (existing) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        return 0;
    }

    SECURITY_DESCRIPTOR sd;
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), &sd, FALSE };
    g_hSharedMap = CreateFileMappingA(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(SharedMemory), SHARED_MEMORY_NAME);
    if (g_hSharedMap) g_pShared = (SharedMemory*)MapViewOfFile(g_hSharedMap, FILE_MAP_WRITE, 0, 0, sizeof(SharedMemory));

    std::thread(InitAudio).detach();

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "SonicCustomBGM_ImGui";
    RegisterClassExA(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    g_hWnd = CreateWindowExA(0, wc.lpszClassName, WINDOW_TITLE,
        WS_OVERLAPPEDWINDOW,
        (sw - WIN_W) / 2, (sh - WIN_H) / 2, WIN_W, WIN_H, NULL, NULL, hInstance, nullptr);

    if (!CreateDeviceD3D(g_hWnd)) {
        CleanupDevice();
        return 1;
    }

    ShowWindow(g_hWnd, SW_SHOW);
    UpdateWindow(g_hWnd);

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) == 0) {
        for (int i = 0; i < SDL_NumJoysticks(); i++) {
            if (SDL_IsGameController(i)) {
                g_gameController = SDL_GameControllerOpen(i);
                if (g_gameController) break;
            }
        }
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0;
    style.FrameRounding = 3;
    style.GrabRounding = 3;
    style.WindowPadding = ImVec2(12, 12);
    style.FramePadding = ImVec2(10, 6);
    style.ItemSpacing = ImVec2(10, 8);
    style.ScrollbarSize = 16;
    style.ScrollbarRounding = 3;

    char winDir[MAX_PATH];
    GetWindowsDirectoryA(winDir, MAX_PATH);
    std::string segoeui = std::string(winDir) + "\\Fonts\\segoeui.ttf";
    std::string segoeuib = std::string(winDir) + "\\Fonts\\segoeuib.ttf";
    std::string consola = std::string(winDir) + "\\Fonts\\consola.ttf";

    g_fontRegular = io.Fonts->AddFontFromFileTTF(segoeui.c_str(), 18.0f);
    if (!g_fontRegular) g_fontRegular = io.Fonts->AddFontDefault();
    g_fontBold = io.Fonts->AddFontFromFileTTF(segoeuib.c_str(), 19.0f);
    if (!g_fontBold) g_fontBold = g_fontRegular;
    g_fontMono = io.Fonts->AddFontFromFileTTF(consola.c_str(), 16.0f);
    if (!g_fontMono) g_fontMono = g_fontRegular;

    ApplyTheme(g_themeIndex);

    ImGui_ImplWin32_Init(g_hWnd);
    ImGui_ImplDX9_Init(g_pd3dDevice);

    std::thread(AudioThread).detach();
    std::thread(AutoInjectThread).detach();

    auto lastHotkeyCheck = std::chrono::steady_clock::now();
    auto lastMuteCheck = std::chrono::steady_clock::now();
    bool themeAppliedAfterLoad = false;

    bool running = true;
    MSG msg;
    while (running) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!running) break;

        if (!themeAppliedAfterLoad && g_settingsLoaded.load()) {
            ApplyTheme(g_themeIndex);
            themeAppliedAfterLoad = true;
        }

        {
            SDL_Event sdle;
            while (SDL_PollEvent(&sdle)) {
                if (sdle.type == SDL_CONTROLLERDEVICEADDED) {
                    if (!g_gameController) g_gameController = SDL_GameControllerOpen(sdle.cdevice.which);
                } else if (sdle.type == SDL_CONTROLLERDEVICEREMOVED) {
                    if (g_gameController && sdle.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_gameController))) {
                        SDL_GameControllerClose(g_gameController);
                        g_gameController = nullptr;
                    }
                }
            }
        }

        PollKeyboardRecording();
        PollControllerRecording();

        if (g_gameGone) {
            DoDisconnect();
            g_gameGone = false;
        }

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastHotkeyCheck).count() >= 50) {
            HandleHotkeys();
            lastHotkeyCheck = now;
        }

        if (g_muteOnUnfocus && g_audioInitialized.load()) {
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastMuteCheck).count() >= 200) {
                DWORD fgPid = 0;
                HWND fg = GetForegroundWindow();
                if (fg) GetWindowThreadProcessId(fg, &fgPid);
                bool unfocused = (fgPid != GetCurrentProcessId() && fgPid != g_targetPid);
                g_audio.SetCategoryVolume(0, unfocused ? 0.0f : g_customBgmVolume.load());
                lastMuteCheck = now;
            }
        }

        if (IsIconic(g_hWnd)) {
            Sleep(50);
            continue;
        }

        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        RenderUI();

        ImGui::Render();
        g_pd3dDevice->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(24, 24, 32), 1.0f, 0);
        if (SUCCEEDED(g_pd3dDevice->BeginScene())) {
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            g_pd3dDevice->EndScene();
        }
        g_pd3dDevice->Present(NULL, NULL, NULL, NULL);
    }

    g_scanRunning = false;
    g_audioThreadRunning = false;
    return (int)msg.wParam;
}
