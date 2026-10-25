#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <windowsx.h>
#include <wrl.h>
#include <mmsystem.h>
#include <shellapi.h>

#include "packages/webview2/build/native/include/WebView2.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <tlhelp32.h>
#include <unordered_map>
#include <vector>
#include <intrin.h>

using namespace std;
using Microsoft::WRL::ComPtr;

constexpr UINT WM_APP_SYNC_STATE = WM_APP + 1;
constexpr UINT WM_APP_RESUME_WEBVIEW = WM_APP + 2;
constexpr UINT WM_APP_TRAYICON = WM_APP + 3;
constexpr LONG kMinimumWindowWidth = 960;
constexpr LONG kMinimumWindowHeight = 680;
constexpr int kWindowCornerRadiusDip = 30;
constexpr UINT kTrayIconId = 1;
constexpr int kTrayMenuShow = 1;
constexpr int kTrayMenuToggleGuard = 2;
constexpr int kTrayMenuExit = 3;

// 每游戏的独立配置（按 g_SelectedGame 索引 0..3）
struct GameProfile {
    bool aceSuppress = true;
    bool weGameSuppress = true;
    bool gameAvoid = true;
    bool steamSuppress = true;
    bool gameFullCore = true;
    bool disableEcores = false;
    bool avoidLastCore = true;  // 游戏进程是否避开最后一个核心
    int scanInterval = 60;
};

// 自定义进程压制项：name 为进程名（如 chrome.exe），priority 0=空闲 1=低于正常 2=正常
struct CustomTarget {
    wstring name;
    int priority = 0;
};

struct AppSettings {
    // ── 全局系统优化（步骤 1） ──
    bool sysPower = true;
    bool sysVbs = true;
    bool sysVsync = true;
    bool edgeSuppress = true;
    bool disableHpet = true;
    bool gameDvr = true;      // 禁用 GameDVR 后台录制
    bool gameMode = true;     // 开启系统游戏模式
    bool qosLimit = false;    // 禁用 QoS 带宽预留（需管理员）
    bool timerRes = true;     // 高精度计时器（1ms）
    bool systemAnim = false;  // 关闭系统动画

    // ── 每游戏配置 ──
    GameProfile profiles[4];

    // ── 自定义进程压制列表 ──
    vector<CustomTarget> customTargets;
} g_Settings;

mutex g_CustomTargetsMutex;

struct EnvBackup {
    DWORD vbsState = 1;
    DWORD hwSchMode = 1;
    DWORD vsyncStateNvidia[5] = {1, 1, 1, 1, 1};
    DWORD gameDvrEnabled = 1;
    DWORD appCaptureEnabled = 1;
    DWORD gameModeEnabled = 1;
    DWORD qosNonBestEffort = 0xFFFFFFFF;
    BOOL animClientArea = TRUE;
    BOOL animMenu = TRUE;
    BOOL animCombo = TRUE;
    BOOL animListSmooth = TRUE;
    BOOL animTooltip = TRUE;
    BOOL animTooltipFade = TRUE;
} g_Backup;

struct ProcessBackupInfo {
    DWORD_PTR affinityMask;
    DWORD priorityClass;
};

atomic<bool> g_IsRunning(false);
atomic<unsigned long> g_RunGeneration(0);
unordered_map<DWORD, ProcessBackupInfo> g_ProcessBackup;
mutex g_ProcessBackupMutex;

HWND g_MainWindow = nullptr;
HANDLE g_HeartbeatStopEvent = nullptr;
thread g_HeartbeatThread;

ComPtr<ICoreWebView2Controller> g_WebViewController;
ComPtr<ICoreWebView2> g_WebView;
ComPtr<ICoreWebView2_12> g_WebView12;
bool g_PageReady = false;
bool g_WindowMaximized = false;
atomic<bool> g_WebViewSuspended{false};
atomic<bool> g_WebViewSuspendPending{false};
atomic<bool> g_WebViewResumeRequested{false};

string g_CpuName;
string g_GpuName;
wstring g_CpuNameW = L"检测中...";
wstring g_GpuNameW = L"检测中...";
wstring g_StatusMessage = L"等待确认...";
wstring g_HtmlFolder;
int g_MemSpeed = 0;
int g_SelectedGame = 0;
bool g_HasHybridCpu = false;
DWORD_PTR g_PCoreAffinityMask = 0;

// ── 配置持久化 / 托盘 / 退出状态 ──
wstring g_ConfigPath;          // 配置文件（lyb_config.ini）
wstring g_BackupPath;          // 环境备份文件（lyb_env_backup.ini）
bool g_ExitRequested = false;  // 用户明确要求退出（托盘菜单），此时关闭窗口才真正退出
bool g_TrayIconAdded = false;

void RequestStateSync();
void ResumeWebViewIfSuspended();
void RestoreEnvironment();
void StartOptimizationGuard();
void StopOptimizationGuard();
bool SetRegDWORD(HKEY rootKey, LPCWSTR subKey, LPCWSTR valueName, DWORD data);

string TrimCopy(const string& value) {
    const size_t start = value.find_first_not_of(" \t\r\n");
    if (start == string::npos) {
        return "";
    }

    const size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

wstring TrimCopyW(const wstring& value) {
    const size_t start = value.find_first_not_of(L" \t\r\n");
    if (start == wstring::npos) {
        return L"";
    }

    const size_t end = value.find_last_not_of(L" \t\r\n");
    return value.substr(start, end - start + 1);
}

wstring AnsiToWide(const string& value) {
    if (value.empty()) {
        return L"";
    }

    const int length = MultiByteToWideChar(CP_ACP, 0, value.c_str(), -1, nullptr, 0);
    if (length <= 0) {
        return L"";
    }

    wstring wide(length, L'\0');
    MultiByteToWideChar(CP_ACP, 0, value.c_str(), -1, wide.data(), length);
    wide.resize(length - 1);
    return wide;
}

wstring GetModuleDirectory() {
    wchar_t buffer[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    wstring path(buffer);
    const size_t slash = path.find_last_of(L"\\/");
    return slash == wstring::npos ? L"." : path.substr(0, slash);
}

// ── 配置持久化 ──
// 配置文件为 UTF-8 文本，逐行 "key=value"。key 采用扁平化命名：
//   global.xxx          → 全局系统优化开关
//   profile{N}.xxx      → 第 N 个游戏（0..3）的配置
//   custom.count        → 自定义进程数量
//   custom.{i}.name     → 自定义进程名
//   custom.{i}.priority → 自定义进程优先级
//   window.maximized    → 窗口是否最大化
//   window.lastGame     → 上次选择的游戏
void SaveSettingsToFile() {
    if (g_ConfigPath.empty()) {
        return;
    }

    wstringstream stream;
    stream << L"global.sysPower=" << (g_Settings.sysPower ? L"1" : L"0") << L"\n";
    stream << L"global.sysVbs=" << (g_Settings.sysVbs ? L"1" : L"0") << L"\n";
    stream << L"global.sysVsync=" << (g_Settings.sysVsync ? L"1" : L"0") << L"\n";
    stream << L"global.edgeSuppress=" << (g_Settings.edgeSuppress ? L"1" : L"0") << L"\n";
    stream << L"global.disableHpet=" << (g_Settings.disableHpet ? L"1" : L"0") << L"\n";
    stream << L"global.gameDvr=" << (g_Settings.gameDvr ? L"1" : L"0") << L"\n";
    stream << L"global.gameMode=" << (g_Settings.gameMode ? L"1" : L"0") << L"\n";
    stream << L"global.qosLimit=" << (g_Settings.qosLimit ? L"1" : L"0") << L"\n";
    stream << L"global.timerRes=" << (g_Settings.timerRes ? L"1" : L"0") << L"\n";
    stream << L"global.systemAnim=" << (g_Settings.systemAnim ? L"1" : L"0") << L"\n";
    for (int i = 0; i < 4; ++i) {
        const GameProfile& profile = g_Settings.profiles[i];
        stream << L"profile" << i << L".aceSuppress=" << (profile.aceSuppress ? L"1" : L"0") << L"\n";
        stream << L"profile" << i << L".weGameSuppress=" << (profile.weGameSuppress ? L"1" : L"0") << L"\n";
        stream << L"profile" << i << L".gameAvoid=" << (profile.gameAvoid ? L"1" : L"0") << L"\n";
        stream << L"profile" << i << L".steamSuppress=" << (profile.steamSuppress ? L"1" : L"0") << L"\n";
        stream << L"profile" << i << L".gameFullCore=" << (profile.gameFullCore ? L"1" : L"0") << L"\n";
        stream << L"profile" << i << L".disableEcores=" << (profile.disableEcores ? L"1" : L"0") << L"\n";
        stream << L"profile" << i << L".avoidLastCore=" << (profile.avoidLastCore ? L"1" : L"0") << L"\n";
        stream << L"profile" << i << L".scanInterval=" << profile.scanInterval << L"\n";
    }
    stream << L"window.maximized=" << (g_WindowMaximized ? L"1" : L"0") << L"\n";
    stream << L"window.lastGame=" << g_SelectedGame << L"\n";

    lock_guard<mutex> lock(g_CustomTargetsMutex);
    stream << L"custom.count=" << g_Settings.customTargets.size() << L"\n";
    for (size_t i = 0; i < g_Settings.customTargets.size(); ++i) {
        stream << L"custom." << i << L".name=" << g_Settings.customTargets[i].name << L"\n";
        stream << L"custom." << i << L".priority=" << g_Settings.customTargets[i].priority << L"\n";
    }
    stream << L"\n";

    const wstring content = stream.str();
    // 转为 UTF-8 写入（进程名等可能含非 ASCII 字符）
    const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, content.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 0) {
        return;
    }
    string utf8(utf8Length - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, content.c_str(), -1, utf8.data(), utf8Length, nullptr, nullptr);

    HANDLE file = CreateFileW(g_ConfigPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(file);
}

void LoadSettingsFromFile() {
    if (g_ConfigPath.empty()) {
        return;
    }

    HANDLE file = CreateFileW(g_ConfigPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    LARGE_INTEGER fileSize = {};
    GetFileSizeEx(file, &fileSize);
    if (fileSize.QuadPart <= 0 || fileSize.QuadPart > 1024 * 1024) {
        CloseHandle(file);
        return;
    }

    string utf8(static_cast<size_t>(fileSize.QuadPart), '\0');
    DWORD bytesRead = 0;
    ReadFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &bytesRead, nullptr);
    CloseHandle(file);
    utf8.resize(bytesRead);

    // 去掉 UTF-8 BOM
    if (utf8.size() >= 3 && static_cast<unsigned char>(utf8[0]) == 0xEF &&
        static_cast<unsigned char>(utf8[1]) == 0xBB && static_cast<unsigned char>(utf8[2]) == 0xBF) {
        utf8.erase(0, 3);
    }

    const int wideLength = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (wideLength <= 0) {
        return;
    }
    wstring content(wideLength - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, content.data(), wideLength);

    unordered_map<wstring, wstring> entries;
    wstringstream stream(content);
    wstring line;
    while (getline(stream, line)) {
        const size_t equals = line.find(L'=');
        if (equals == wstring::npos) {
            continue;
        }
        const wstring key = TrimCopyW(line.substr(0, equals));
        const wstring value = TrimCopyW(line.substr(equals + 1));
        if (!key.empty()) {
            entries[key] = value;
        }
    }

    auto readBool = [&entries](const wchar_t* key, bool fallback) {
        const auto it = entries.find(key);
        if (it == entries.end()) {
            return fallback;
        }
        return it->second == L"1";
    };
    auto readInt = [&entries](const wchar_t* key, int fallback) {
        const auto it = entries.find(key);
        if (it == entries.end()) {
            return fallback;
        }
        return _wtoi(it->second.c_str());
    };

    g_Settings.sysPower = readBool(L"global.sysPower", g_Settings.sysPower);
    g_Settings.sysVbs = readBool(L"global.sysVbs", g_Settings.sysVbs);
    g_Settings.sysVsync = readBool(L"global.sysVsync", g_Settings.sysVsync);
    g_Settings.edgeSuppress = readBool(L"global.edgeSuppress", g_Settings.edgeSuppress);
    g_Settings.disableHpet = readBool(L"global.disableHpet", g_Settings.disableHpet);
    g_Settings.gameDvr = readBool(L"global.gameDvr", g_Settings.gameDvr);
    g_Settings.gameMode = readBool(L"global.gameMode", g_Settings.gameMode);
    g_Settings.qosLimit = readBool(L"global.qosLimit", g_Settings.qosLimit);
    g_Settings.timerRes = readBool(L"global.timerRes", g_Settings.timerRes);
    g_Settings.systemAnim = readBool(L"global.systemAnim", g_Settings.systemAnim);

    for (int i = 0; i < 4; ++i) {
        GameProfile& profile = g_Settings.profiles[i];
        wchar_t key[64] = {};
        swprintf_s(key, L"profile%d.aceSuppress", i);
        profile.aceSuppress = readBool(key, profile.aceSuppress);
        swprintf_s(key, L"profile%d.weGameSuppress", i);
        profile.weGameSuppress = readBool(key, profile.weGameSuppress);
        swprintf_s(key, L"profile%d.gameAvoid", i);
        profile.gameAvoid = readBool(key, profile.gameAvoid);
        swprintf_s(key, L"profile%d.steamSuppress", i);
        profile.steamSuppress = readBool(key, profile.steamSuppress);
        swprintf_s(key, L"profile%d.gameFullCore", i);
        profile.gameFullCore = readBool(key, profile.gameFullCore);
        swprintf_s(key, L"profile%d.disableEcores", i);
        profile.disableEcores = readBool(key, profile.disableEcores);
        swprintf_s(key, L"profile%d.avoidLastCore", i);
        profile.avoidLastCore = readBool(key, profile.avoidLastCore);
        swprintf_s(key, L"profile%d.scanInterval", i);
        profile.scanInterval = max(5, min(300, readInt(key, profile.scanInterval)));
    }

    const int savedGame = readInt(L"window.lastGame", g_SelectedGame);
    if (savedGame >= 0 && savedGame < 4) {
        g_SelectedGame = savedGame;
    }

    lock_guard<mutex> lock(g_CustomTargetsMutex);
    const int customCount = max(0, min(32, readInt(L"custom.count", 0)));
    g_Settings.customTargets.clear();
    for (int i = 0; i < customCount; ++i) {
        wchar_t key[64] = {};
        swprintf_s(key, L"custom.%d.name", i);
        const auto nameIt = entries.find(key);
        if (nameIt == entries.end() || nameIt->second.empty()) {
            continue;
        }
        swprintf_s(key, L"custom.%d.priority", i);
        CustomTarget target;
        target.name = nameIt->second;
        target.priority = max(0, min(2, readInt(key, 0)));
        g_Settings.customTargets.push_back(target);
    }
}

// ── 环境备份持久化：崩溃/强杀后自动恢复系统设置 ──
// 守护启动时把备份写入磁盘文件；正常停止时删除。下次启动若发现备份文件
// 仍然存在，说明上次进程未正常退出（崩溃/被强杀），此时加载备份并恢复环境。
void SaveEnvironmentBackupToFile() {
    if (g_BackupPath.empty()) {
        return;
    }
    wstringstream stream;
    stream << L"vbsState=" << g_Backup.vbsState << L"\n";
    stream << L"hwSchMode=" << g_Backup.hwSchMode << L"\n";
    for (int i = 0; i < 5; ++i) {
        stream << L"vsync" << i << L"=" << g_Backup.vsyncStateNvidia[i] << L"\n";
    }
    stream << L"gameDvrEnabled=" << g_Backup.gameDvrEnabled << L"\n";
    stream << L"appCaptureEnabled=" << g_Backup.appCaptureEnabled << L"\n";
    stream << L"gameModeEnabled=" << g_Backup.gameModeEnabled << L"\n";
    stream << L"qosNonBestEffort=" << g_Backup.qosNonBestEffort << L"\n";
    stream << L"animClientArea=" << (g_Backup.animClientArea ? 1 : 0) << L"\n";
    stream << L"animMenu=" << (g_Backup.animMenu ? 1 : 0) << L"\n";
    stream << L"animCombo=" << (g_Backup.animCombo ? 1 : 0) << L"\n";
    stream << L"animListSmooth=" << (g_Backup.animListSmooth ? 1 : 0) << L"\n";
    stream << L"animTooltip=" << (g_Backup.animTooltip ? 1 : 0) << L"\n";
    stream << L"animTooltipFade=" << (g_Backup.animTooltipFade ? 1 : 0) << L"\n";
    // 记录启用了哪些优化，恢复时按需还原
    stream << L"applied.sysPower=" << (g_Settings.sysPower ? 1 : 0) << L"\n";
    stream << L"applied.sysVbs=" << (g_Settings.sysVbs ? 1 : 0) << L"\n";
    stream << L"applied.sysVsync=" << (g_Settings.sysVsync ? 1 : 0) << L"\n";
    stream << L"applied.gameDvr=" << (g_Settings.gameDvr ? 1 : 0) << L"\n";
    stream << L"applied.gameMode=" << (g_Settings.gameMode ? 1 : 0) << L"\n";
    stream << L"applied.qosLimit=" << (g_Settings.qosLimit ? 1 : 0) << L"\n";
    stream << L"applied.timerRes=" << (g_Settings.timerRes ? 1 : 0) << L"\n";
    stream << L"applied.systemAnim=" << (g_Settings.systemAnim ? 1 : 0) << L"\n";

    const wstring content = stream.str();
    const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, content.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 0) {
        return;
    }
    string utf8(utf8Length - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, content.c_str(), -1, utf8.data(), utf8Length, nullptr, nullptr);

    HANDLE file = CreateFileW(g_BackupPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(file);
}

// 检查是否有上次未清理的环境备份：有则恢复并删除，返回是否发生了恢复
bool RecoverEnvironmentFromCrash() {
    if (g_BackupPath.empty()) {
        return false;
    }
    if (GetFileAttributesW(g_BackupPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false;
    }

    HANDLE file = CreateFileW(g_BackupPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER fileSize = {};
    GetFileSizeEx(file, &fileSize);
    if (fileSize.QuadPart <= 0 || fileSize.QuadPart > 1024 * 1024) {
        CloseHandle(file);
        DeleteFileW(g_BackupPath.c_str());
        return false;
    }
    string utf8(static_cast<size_t>(fileSize.QuadPart), '\0');
    DWORD bytesRead = 0;
    ReadFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &bytesRead, nullptr);
    CloseHandle(file);
    utf8.resize(bytesRead);
    if (utf8.size() >= 3 && static_cast<unsigned char>(utf8[0]) == 0xEF &&
        static_cast<unsigned char>(utf8[1]) == 0xBB && static_cast<unsigned char>(utf8[2]) == 0xBF) {
        utf8.erase(0, 3);
    }
    const int wideLength = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (wideLength <= 0) {
        DeleteFileW(g_BackupPath.c_str());
        return false;
    }
    wstring content(wideLength - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, content.data(), wideLength);

    unordered_map<wstring, wstring> entries;
    wstringstream stream(content);
    wstring line;
    while (getline(stream, line)) {
        const size_t equals = line.find(L'=');
        if (equals == wstring::npos) {
            continue;
        }
        entries[TrimCopyW(line.substr(0, equals))] = TrimCopyW(line.substr(equals + 1));
    }
    auto readInt = [&entries](const wchar_t* key, int fallback) {
        const auto it = entries.find(key);
        return it == entries.end() ? fallback : _wtoi(it->second.c_str());
    };

    // 先按备份还原 g_Backup，再按 applied.* 还原系统设置
    const bool wasPower = readInt(L"applied.sysPower", 0) == 1;
    const bool wasVbs = readInt(L"applied.sysVbs", 0) == 1;
    const bool wasVsync = readInt(L"applied.sysVsync", 0) == 1;
    const bool wasDvr = readInt(L"applied.gameDvr", 0) == 1;
    const bool wasMode = readInt(L"applied.gameMode", 0) == 1;
    const bool wasQos = readInt(L"applied.qosLimit", 0) == 1;
    const bool wasTimer = readInt(L"applied.timerRes", 0) == 1;
    const bool wasAnim = readInt(L"applied.systemAnim", 0) == 1;

    g_Backup.vbsState = static_cast<DWORD>(readInt(L"vbsState", 1));
    g_Backup.hwSchMode = static_cast<DWORD>(readInt(L"hwSchMode", 1));
    for (int i = 0; i < 5; ++i) {
        wchar_t key[32] = {};
        swprintf_s(key, L"vsync%d", i);
        g_Backup.vsyncStateNvidia[i] = static_cast<DWORD>(readInt(key, 1));
    }
    g_Backup.gameDvrEnabled = static_cast<DWORD>(readInt(L"gameDvrEnabled", 1));
    g_Backup.appCaptureEnabled = static_cast<DWORD>(readInt(L"appCaptureEnabled", 1));
    g_Backup.gameModeEnabled = static_cast<DWORD>(readInt(L"gameModeEnabled", 1));
    g_Backup.qosNonBestEffort = static_cast<DWORD>(readInt(L"qosNonBestEffort", 0xFFFFFFFF));
    g_Backup.animClientArea = readInt(L"animClientArea", 1) == 1;
    g_Backup.animMenu = readInt(L"animMenu", 1) == 1;
    g_Backup.animCombo = readInt(L"animCombo", 1) == 1;
    g_Backup.animListSmooth = readInt(L"animListSmooth", 1) == 1;
    g_Backup.animTooltip = readInt(L"animTooltip", 1) == 1;
    g_Backup.animTooltipFade = readInt(L"animTooltipFade", 1) == 1;

    // 临时启用对应开关，让 RestoreEnvironment 按需还原
    const bool oldPower = g_Settings.sysPower;
    const bool oldVbs = g_Settings.sysVbs;
    const bool oldVsync = g_Settings.sysVsync;
    const bool oldDvr = g_Settings.gameDvr;
    const bool oldMode = g_Settings.gameMode;
    const bool oldQos = g_Settings.qosLimit;
    const bool oldTimer = g_Settings.timerRes;
    const bool oldAnim = g_Settings.systemAnim;
    g_Settings.sysPower = wasPower;
    g_Settings.sysVbs = wasVbs;
    g_Settings.sysVsync = wasVsync;
    g_Settings.gameDvr = wasDvr;
    g_Settings.gameMode = wasMode;
    g_Settings.qosLimit = wasQos;
    g_Settings.timerRes = wasTimer;
    g_Settings.systemAnim = wasAnim;

    RestoreEnvironment();

    // 启动早期 g_GpuName 尚未检测，RestoreEnvironment 的 NVIDIA vsync 分支不会执行；
    // 崩溃前若启用了 vsync 优化，这里直接按备份值还原
    if (wasVsync) {
        const wstring basePath = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\";
        for (int i = 0; i < 5; ++i) {
            const wstring subKey = basePath + to_wstring(i);
            SetRegDWORD(HKEY_LOCAL_MACHINE, subKey.c_str(), L"VSyncMode", g_Backup.vsyncStateNvidia[i]);
        }
    }

    g_Settings.sysPower = oldPower;
    g_Settings.sysVbs = oldVbs;
    g_Settings.sysVsync = oldVsync;
    g_Settings.gameDvr = oldDvr;
    g_Settings.gameMode = oldMode;
    g_Settings.qosLimit = oldQos;
    g_Settings.timerRes = oldTimer;
    g_Settings.systemAnim = oldAnim;

    DeleteFileW(g_BackupPath.c_str());
    return true;
}

// ── 托盘图标 ──
void AddTrayIcon(HWND hwnd) {
    if (g_TrayIconAdded) {
        return;
    }
    NOTIFYICONDATAW data = {};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd;
    data.uID = kTrayIconId;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = WM_APP_TRAYICON;
    data.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCE(1));
    wcscpy_s(data.szTip, L"LYB 引擎 2.0");
    if (Shell_NotifyIconW(NIM_ADD, &data)) {
        g_TrayIconAdded = true;
    }
}

void RemoveTrayIcon() {
    if (!g_TrayIconAdded) {
        return;
    }
    NOTIFYICONDATAW data = {};
    data.cbSize = sizeof(data);
    data.hWnd = g_MainWindow;
    data.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
    g_TrayIconAdded = false;
}

void ShowTrayMenu() {
    if (g_MainWindow == nullptr) {
        return;
    }
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kTrayMenuShow, L"显示主窗口");
    AppendMenuW(menu, MF_STRING, kTrayMenuToggleGuard, g_IsRunning.load() ? L"停止守护" : L"开始守护");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayMenuExit, L"退出");

    POINT cursor = {};
    GetCursorPos(&cursor);
    SetForegroundWindow(g_MainWindow);
    const int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, cursor.x, cursor.y, 0, g_MainWindow, nullptr);
    DestroyMenu(menu);

    if (command == kTrayMenuShow) {
        ShowWindow(g_MainWindow, SW_RESTORE);
        ShowWindow(g_MainWindow, SW_SHOW);
        SetForegroundWindow(g_MainWindow);
    } else if (command == kTrayMenuToggleGuard) {
        if (g_IsRunning.load()) {
            StopOptimizationGuard();
        } else {
            StartOptimizationGuard();
        }
    } else if (command == kTrayMenuExit) {
        g_ExitRequested = true;
        PostMessageW(g_MainWindow, WM_CLOSE, 0, 0);
    }
}

void EnablePerMonitorDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        using SetDpiAwarenessContextFn = BOOL (WINAPI*)(DPI_AWARENESS_CONTEXT);
        const auto setDpiAwarenessContext =
            reinterpret_cast<SetDpiAwarenessContextFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setDpiAwarenessContext != nullptr &&
            setDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
            return;
        }

        using SetProcessDpiAwareFn = BOOL (WINAPI*)();
        const auto setProcessDpiAware =
            reinterpret_cast<SetProcessDpiAwareFn>(GetProcAddress(user32, "SetProcessDPIAware"));
        if (setProcessDpiAware != nullptr) {
            setProcessDpiAware();
        }
    }
}

UINT GetWindowDpiSafe(HWND hwnd) {
    if (hwnd != nullptr) {
        return GetDpiForWindow(hwnd);
    }
    return USER_DEFAULT_SCREEN_DPI;
}

int GetSystemMetricForWindow(int index, HWND hwnd) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
        using GetSystemMetricsForDpiFn = int (WINAPI*)(int, UINT);
        const auto getSystemMetricsForDpi =
            reinterpret_cast<GetSystemMetricsForDpiFn>(GetProcAddress(user32, "GetSystemMetricsForDpi"));
        if (getSystemMetricsForDpi != nullptr) {
            return getSystemMetricsForDpi(index, GetWindowDpiSafe(hwnd));
        }
    }
    return GetSystemMetrics(index);
}

int GetWindowCornerRadiusPx(HWND hwnd) {
    return max(18, MulDiv(kWindowCornerRadiusDip, GetWindowDpiSafe(hwnd), USER_DEFAULT_SCREEN_DPI));
}

void ApplyWindowCornerRegion() {
    if (g_MainWindow == nullptr) {
        return;
    }

    if (IsZoomed(g_MainWindow) != FALSE) {
        SetWindowRgn(g_MainWindow, nullptr, TRUE);
        return;
    }

    RECT windowRect = {};
    GetWindowRect(g_MainWindow, &windowRect);

    const int width = max<LONG>(1, windowRect.right - windowRect.left);
    const int height = max<LONG>(1, windowRect.bottom - windowRect.top);
    const int radius = GetWindowCornerRadiusPx(g_MainWindow);

    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius, radius);
    if (region == nullptr) {
        return;
    }

    if (SetWindowRgn(g_MainWindow, region, TRUE) == 0) {
        DeleteObject(region);
    }
}

void PaintStartupPlaceholder(HDC hdc, const RECT& clientRect) {
    const UINT dpi = GetWindowDpiSafe(g_MainWindow);
    const int pad = MulDiv(28, dpi, USER_DEFAULT_SCREEN_DPI);
    const int topBarHeight = MulDiv(96, dpi, USER_DEFAULT_SCREEN_DPI);
    const int panelWidth = min<int>(clientRect.right - clientRect.left - pad * 2, MulDiv(620, dpi, USER_DEFAULT_SCREEN_DPI));
    const int panelHeight = MulDiv(150, dpi, USER_DEFAULT_SCREEN_DPI);
    const int panelOffsetX = static_cast<int>(((clientRect.right - clientRect.left) - panelWidth) / 2);
    const int panelOffsetY = static_cast<int>(((clientRect.bottom - clientRect.top) - panelHeight) / 2);
    const int panelLeft = clientRect.left + (panelOffsetX > 0 ? panelOffsetX : 0);
    const int panelTop = clientRect.top + (panelOffsetY > 0 ? panelOffsetY : 0) + MulDiv(24, dpi, USER_DEFAULT_SCREEN_DPI);
    const int panelRadius = GetWindowCornerRadiusPx(g_MainWindow);

    HBRUSH bgBrush = CreateSolidBrush(RGB(4, 11, 22));
    FillRect(hdc, &clientRect, bgBrush);
    DeleteObject(bgBrush);

    RECT topBar = clientRect;
    topBar.bottom = min(clientRect.bottom, clientRect.top + topBarHeight);
    HBRUSH topBrush = CreateSolidBrush(RGB(8, 16, 29));
    FillRect(hdc, &topBar, topBrush);
    DeleteObject(topBrush);

    RECT panelRect = {panelLeft, panelTop, panelLeft + panelWidth, panelTop + panelHeight};
    HBRUSH panelBrush = CreateSolidBrush(RGB(10, 19, 34));
    HPEN panelPen = CreatePen(PS_SOLID, 1, RGB(32, 54, 78));
    HGDIOBJ oldBrush = SelectObject(hdc, panelBrush);
    HGDIOBJ oldPen = SelectObject(hdc, panelPen);
    RoundRect(hdc, panelRect.left, panelRect.top, panelRect.right, panelRect.bottom, panelRadius, panelRadius);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(panelBrush);
    DeleteObject(panelPen);

    SetBkMode(hdc, TRANSPARENT);

    HFONT titleFont = CreateFontW(
        -MulDiv(30, dpi, USER_DEFAULT_SCREEN_DPI), 0, 0, 0, FW_HEAVY,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HFONT subFont = CreateFontW(
        -MulDiv(12, dpi, USER_DEFAULT_SCREEN_DPI), 0, 0, 0, FW_MEDIUM,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HFONT statusFont = CreateFontW(
        -MulDiv(24, dpi, USER_DEFAULT_SCREEN_DPI), 0, 0, 0, FW_BOLD,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HFONT noteFont = CreateFontW(
        -MulDiv(13, dpi, USER_DEFAULT_SCREEN_DPI), 0, 0, 0, FW_MEDIUM,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_OUTLINE_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");

    RECT titleRect = {pad, clientRect.top + MulDiv(18, dpi, USER_DEFAULT_SCREEN_DPI), clientRect.right - pad, topBar.bottom};
    SelectObject(hdc, titleFont);
    SetTextColor(hdc, RGB(239, 246, 255));
    DrawTextW(hdc, L"LYB Engine 2.0", -1, &titleRect, DT_LEFT | DT_TOP | DT_SINGLELINE);

    RECT subRect = {pad, clientRect.top + MulDiv(58, dpi, USER_DEFAULT_SCREEN_DPI), clientRect.right - pad, topBar.bottom};
    SelectObject(hdc, subFont);
    SetTextColor(hdc, RGB(147, 197, 253));
    DrawTextW(hdc, L"\u6B63\u5728\u521D\u59CB\u5316 WebView2 \u754C\u9762", -1, &subRect, DT_LEFT | DT_TOP | DT_SINGLELINE);

    RECT statusRect = {panelRect.left + pad, panelRect.top + MulDiv(28, dpi, USER_DEFAULT_SCREEN_DPI), panelRect.right - pad, panelRect.bottom};
    SelectObject(hdc, statusFont);
    SetTextColor(hdc, RGB(255, 255, 255));
    DrawTextW(hdc, L"\u6B63\u5728\u52A0\u8F7D\u754C\u9762...", -1, &statusRect, DT_CENTER | DT_TOP | DT_SINGLELINE);

    RECT noteRect = {panelRect.left + pad, panelRect.top + MulDiv(76, dpi, USER_DEFAULT_SCREEN_DPI), panelRect.right - pad, panelRect.bottom - pad};
    SelectObject(hdc, noteFont);
    SetTextColor(hdc, RGB(159, 180, 201));
    DrawTextW(hdc, L"\u786C\u4EF6\u68C0\u6D4B\u5DF2\u6539\u4E3A\u540E\u53F0\u5F02\u6B65\u52A0\u8F7D\uFF0C\u9996\u6B21\u542F\u52A8\u5C06\u66F4\u5FEB\u663E\u793A\u754C\u9762\u3002", -1, &noteRect, DT_CENTER | DT_WORDBREAK);

    DeleteObject(titleFont);
    DeleteObject(subFont);
    DeleteObject(statusFont);
    DeleteObject(noteFont);
}

bool IsMainWindowMaximized() {
    return g_MainWindow != nullptr && IsZoomed(g_MainWindow) != FALSE;
}

void UpdateWindowStateSnapshot(bool forceSync = false) {
    const bool nextState = IsMainWindowMaximized();
    if (forceSync || nextState != g_WindowMaximized) {
        g_WindowMaximized = nextState;
        RequestStateSync();
    }
}

void StartWindowDrag() {
    if (g_MainWindow == nullptr) {
        return;
    }

    if (IsMainWindowMaximized()) {
        ShowWindow(g_MainWindow, SW_RESTORE);
        UpdateWindowStateSnapshot(true);
    }

    ReleaseCapture();
    SendMessageW(g_MainWindow, WM_NCLBUTTONDOWN, HTCAPTION, 0);
}

void ToggleMainWindowMaximize() {
    if (g_MainWindow == nullptr) {
        return;
    }

    ShowWindow(g_MainWindow, IsMainWindowMaximized() ? SW_RESTORE : SW_MAXIMIZE);
    UpdateWindowStateSnapshot(true);
}

void HandleWindowCommand(const wstring& command) {
    if (g_MainWindow == nullptr) {
        return;
    }

    if (command == L"drag") {
        StartWindowDrag();
        return;
    }

    if (command == L"minimize") {
        ShowWindow(g_MainWindow, SW_MINIMIZE);
        return;
    }

    if (command == L"maximize") {
        ShowWindow(g_MainWindow, SW_MAXIMIZE);
        UpdateWindowStateSnapshot(true);
        return;
    }

    if (command == L"restore") {
        ShowWindow(g_MainWindow, SW_RESTORE);
        UpdateWindowStateSnapshot(true);
        return;
    }

    if (command == L"toggleMaximize") {
        ToggleMainWindowMaximize();
        return;
    }

    if (command == L"close") {
        PostMessageW(g_MainWindow, WM_CLOSE, 0, 0);
    }
}

void ApplyWindowMinMaxInfo(HWND hwnd, MINMAXINFO* info) {
    if (info == nullptr) {
        return;
    }

    info->ptMinTrackSize.x = kMinimumWindowWidth;
    info->ptMinTrackSize.y = kMinimumWindowHeight;

    MONITORINFO monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (monitor != nullptr && GetMonitorInfoW(monitor, &monitorInfo)) {
        info->ptMaxPosition.x = monitorInfo.rcWork.left - monitorInfo.rcMonitor.left;
        info->ptMaxPosition.y = monitorInfo.rcWork.top - monitorInfo.rcMonitor.top;
        info->ptMaxSize.x = monitorInfo.rcWork.right - monitorInfo.rcWork.left;
        info->ptMaxSize.y = monitorInfo.rcWork.bottom - monitorInfo.rcWork.top;
    }
}

LRESULT HitTestResizeBorder(HWND hwnd, LPARAM lParam) {
    if (IsZoomed(hwnd)) {
        return HTCLIENT;
    }

    RECT windowRect = {};
    GetWindowRect(hwnd, &windowRect);

    const LONG borderX = max<LONG>(8, GetSystemMetricForWindow(SM_CXSIZEFRAME, hwnd) + GetSystemMetricForWindow(SM_CXPADDEDBORDER, hwnd));
    const LONG borderY = max<LONG>(8, GetSystemMetricForWindow(SM_CYSIZEFRAME, hwnd) + GetSystemMetricForWindow(SM_CXPADDEDBORDER, hwnd));
    const LONG x = static_cast<LONG>(static_cast<SHORT>(LOWORD(lParam)));
    const LONG y = static_cast<LONG>(static_cast<SHORT>(HIWORD(lParam)));

    const bool isLeft = x >= windowRect.left && x < windowRect.left + borderX;
    const bool isRight = x < windowRect.right && x >= windowRect.right - borderX;
    const bool isTop = y >= windowRect.top && y < windowRect.top + borderY;
    const bool isBottom = y < windowRect.bottom && y >= windowRect.bottom - borderY;

    if (isTop && isLeft) return HTTOPLEFT;
    if (isTop && isRight) return HTTOPRIGHT;
    if (isBottom && isLeft) return HTBOTTOMLEFT;
    if (isBottom && isRight) return HTBOTTOMRIGHT;
    if (isTop) return HTTOP;
    if (isBottom) return HTBOTTOM;
    if (isLeft) return HTLEFT;
    if (isRight) return HTRIGHT;
    return HTCLIENT;
}

wstring JsonEscape(const wstring& value) {
    wstring escaped;
    escaped.reserve(value.size() + 16);

    for (const wchar_t ch : value) {
        switch (ch) {
            case L'\\': escaped += L"\\\\"; break;
            case L'"': escaped += L"\\\""; break;
            case L'\n': escaped += L"\\n"; break;
            case L'\r': escaped += L"\\r"; break;
            case L'\t': escaped += L"\\t"; break;
            default:
                if (ch >= 0 && ch < 0x20) {
                    wchar_t encoded[7] = {0};
                    swprintf_s(encoded, L"\\u%04x", static_cast<unsigned int>(ch));
                    escaped += encoded;
                } else {
                    escaped += ch;
                }
                break;
        }
    }

    return escaped;
}

wstring HtmlEscape(const wstring& value) {
    wstring escaped;
    escaped.reserve(value.size() + 16);

    for (const wchar_t ch : value) {
        switch (ch) {
            case L'&': escaped += L"&amp;"; break;
            case L'<': escaped += L"&lt;"; break;
            case L'>': escaped += L"&gt;"; break;
            case L'"': escaped += L"&quot;"; break;
            default: escaped += ch; break;
        }
    }

    return escaped;
}

const wchar_t* BoolJson(bool value) {
    return value ? L"true" : L"false";
}

const wchar_t* GameIdFromSelection(int selection) {
    switch (selection) {
        case 0: return L"valorant";
        case 1: return L"delta";
        case 2: return L"cs2";
        case 3: return L"pubg";
        default: return L"valorant";
    }
}

int GameSelectionFromId(const wstring& gameId) {
    if (gameId == L"valorant") return 0;
    if (gameId == L"delta") return 1;
    if (gameId == L"cs2") return 2;
    if (gameId == L"pubg") return 3;
    return -1;
}

bool IsTencentGame(int selection) {
    return selection == 0 || selection == 1;
}

bool IsIntelCpu() {
    return g_CpuName.find("Intel") != string::npos;
}

DWORD_PTR GetHighestSetBitMask(DWORD_PTR mask) {
    if (mask == 0) {
        return static_cast<DWORD_PTR>(1);
    }

    DWORD_PTR bit = static_cast<DWORD_PTR>(1) << (sizeof(DWORD_PTR) * 8 - 1);
    while (bit != 0 && (mask & bit) == 0) {
        bit >>= 1;
    }

    return bit != 0 ? bit : static_cast<DWORD_PTR>(1);
}

void RequestStateSync() {
    if (g_MainWindow != nullptr && IsWindow(g_MainWindow) != FALSE) {
        PostMessageW(g_MainWindow, WM_APP_SYNC_STATE, 0, 0);
    }
}

wstring BuildStateJson() {
    const bool hpetAvailable = !IsIntelCpu();
    const bool disableHpetChecked = hpetAvailable && g_Settings.disableHpet;
    const wstring memoryLabel = g_MemSpeed > 0 ? to_wstring(g_MemSpeed) + L" MHz" : L"不可用";
    const UINT windowDpi = GetWindowDpiSafe(g_MainWindow);
    const GameProfile& profile = g_Settings.profiles[g_SelectedGame];

    wstring json = L"{\"type\":\"state\",\"payload\":{";
    json += L"\"running\":";
    json += BoolJson(g_IsRunning.load());
    json += L",\"selectedGame\":\"";
    json += GameIdFromSelection(g_SelectedGame);
    json += L"\",\"statusText\":\"";
    json += JsonEscape(g_StatusMessage);
    json += L"\",\"hardware\":{";
    json += L"\"cpu\":\"";
    json += JsonEscape(g_CpuNameW);
    json += L"\",\"gpu\":\"";
    json += JsonEscape(g_GpuNameW);
    json += L"\",\"ramSpeed\":";
    json += g_MemSpeed > 0 ? to_wstring(g_MemSpeed) : wstring(L"0");
    json += L",\"memoryLabel\":\"";
    json += JsonEscape(memoryLabel);
    json += L"\",\"hybrid\":";
    json += BoolJson(g_HasHybridCpu);
    json += L",\"hpetAvailable\":";
    json += BoolJson(hpetAvailable);
    json += L"},\"window\":{";
    json += L"\"maximized\":";
    json += BoolJson(g_WindowMaximized);
    json += L",\"dpi\":";
    json += to_wstring(windowDpi);
    json += L"},\"settings\":{";
    json += L"\"sysPower\":";
    json += BoolJson(g_Settings.sysPower);
    json += L",\"sysVbs\":";
    json += BoolJson(g_Settings.sysVbs);
    json += L",\"sysVsync\":";
    json += BoolJson(g_Settings.sysVsync);
    json += L",\"edgeSuppress\":";
    json += BoolJson(g_Settings.edgeSuppress);
    json += L",\"disableHpet\":";
    json += BoolJson(disableHpetChecked);
    json += L",\"gameDvr\":";
    json += BoolJson(g_Settings.gameDvr);
    json += L",\"gameMode\":";
    json += BoolJson(g_Settings.gameMode);
    json += L",\"qosLimit\":";
    json += BoolJson(g_Settings.qosLimit);
    json += L",\"timerRes\":";
    json += BoolJson(g_Settings.timerRes);
    json += L",\"systemAnim\":";
    json += BoolJson(g_Settings.systemAnim);
    json += L",\"aceSuppress\":";
    json += BoolJson(profile.aceSuppress);
    json += L",\"weGameSuppress\":";
    json += BoolJson(profile.weGameSuppress);
    json += L",\"gameAvoid\":";
    json += BoolJson(profile.gameAvoid);
    json += L",\"steamSuppress\":";
    json += BoolJson(profile.steamSuppress);
    json += L",\"gameFullCore\":";
    json += BoolJson(profile.gameFullCore);
    json += L",\"disableEcores\":";
    json += BoolJson(g_HasHybridCpu && profile.disableEcores);
    json += L",\"avoidLastCore\":";
    json += BoolJson(profile.avoidLastCore);
    json += L",\"scanInterval\":";
    json += to_wstring(profile.scanInterval);
    json += L",\"customTargets\":[";
    {
        lock_guard<mutex> lock(g_CustomTargetsMutex);
        for (size_t i = 0; i < g_Settings.customTargets.size(); ++i) {
            if (i > 0) {
                json += L",";
            }
            json += L"{\"name\":\"";
            json += JsonEscape(g_Settings.customTargets[i].name);
            json += L"\",\"priority\":";
            json += to_wstring(g_Settings.customTargets[i].priority);
            json += L"}";
        }
    }
    json += L"]}}}";
    return json;
}

void PushStateToWebView() {
    if (!g_PageReady || !g_WebView) {
        return;
    }

    // 若 WebView 已挂起（或挂起请求进行中），先恢复再推送，避免消息丢失
    if (g_WebViewSuspended.load() || g_WebViewSuspendPending.load()) {
        ResumeWebViewIfSuspended();
    }

    const wstring payload = BuildStateJson();
    g_WebView->PostWebMessageAsJson(payload.c_str());
}

void EnableDebugPrivilege() {
    HANDLE token = nullptr;
    TOKEN_PRIVILEGES privileges = {};
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &privileges.Privileges[0].Luid);
        privileges.PrivilegeCount = 1;
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
        CloseHandle(token);
    }
}

string GetCPUName() {
    int cpuInfo[4] = {-1, -1, -1, -1};
    char cpuBrand[0x40] = {0};
    __cpuid(cpuInfo, 0x80000000);
    if (static_cast<unsigned int>(cpuInfo[0]) >= 0x80000004) {
        __cpuid(cpuInfo, 0x80000002);
        memcpy(cpuBrand, cpuInfo, sizeof(cpuInfo));
        __cpuid(cpuInfo, 0x80000003);
        memcpy(cpuBrand + 16, cpuInfo, sizeof(cpuInfo));
        __cpuid(cpuInfo, 0x80000004);
        memcpy(cpuBrand + 32, cpuInfo, sizeof(cpuInfo));
    }
    return TrimCopy(cpuBrand);
}

string GetGPUName() {
    DISPLAY_DEVICEA device = {sizeof(device)};
    if (EnumDisplayDevicesA(nullptr, 0, &device, EDD_GET_DEVICE_INTERFACE_NAME)) {
        return TrimCopy(device.DeviceString);
    }
    return "Unknown GPU";
}

int GetMemorySpeedSafe() {
    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &attributes, 0)) {
        return 0;
    }

    STARTUPINFOW startupInfo = {sizeof(startupInfo)};
    startupInfo.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    startupInfo.wShowWindow = SW_HIDE;
    startupInfo.hStdOutput = writePipe;
    startupInfo.hStdError = writePipe;

    PROCESS_INFORMATION processInfo = {};
    wchar_t command[] = L"wmic memorychip get speed";

    if (CreateProcessW(nullptr, command, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startupInfo, &processInfo)) {
        WaitForSingleObject(processInfo.hProcess, 2000);
        CloseHandle(processInfo.hProcess);
        CloseHandle(processInfo.hThread);
    }

    CloseHandle(writePipe);

    char buffer[1024] = {0};
    DWORD bytesRead = 0;
    if (ReadFile(readPipe, buffer, sizeof(buffer) - 1, &bytesRead, nullptr)) {
        buffer[bytesRead] = '\0';
    }
    CloseHandle(readPipe);

    int maxSpeed = 0;
    string output(buffer);
    stringstream stream(output);
    string token;
    while (stream >> token) {
        const int speed = atoi(token.c_str());
        if (speed > maxSpeed) {
            maxSpeed = speed;
        }
    }

    return maxSpeed;
}

void DetectHeterogeneousCores() {
    g_HasHybridCpu = false;
    g_PCoreAffinityMask = 0;

    SYSTEM_INFO systemInfo = {};
    GetSystemInfo(&systemInfo);

    const bool likelyIntelHybrid =
        (g_CpuName.find("Intel") != string::npos) &&
        (g_CpuName.find("12th") != string::npos ||
         g_CpuName.find("13th") != string::npos ||
         g_CpuName.find("14th") != string::npos ||
         g_CpuName.find("15th") != string::npos ||
         g_CpuName.find("Core Ultra") != string::npos);

    const bool likelyAmdHybrid =
        (g_CpuName.find("Ryzen AI") != string::npos) ||
        (g_CpuName.find("Strix Point") != string::npos);

    bool strictIntelHybrid = false;
    if (likelyIntelHybrid) {
        if (g_CpuName.find("Core Ultra") != string::npos) {
            strictIntelHybrid = true;
        } else {
            strictIntelHybrid = systemInfo.dwNumberOfProcessors >= 14;
        }
    }

    g_HasHybridCpu = strictIntelHybrid || (likelyAmdHybrid && systemInfo.dwNumberOfProcessors >= 8);
    if (g_HasHybridCpu && systemInfo.dwNumberOfProcessors > 2) {
        const DWORD processorCount = min<DWORD>(systemInfo.dwNumberOfProcessors - 1, static_cast<DWORD>(sizeof(DWORD_PTR) * 8 - 1));
        g_PCoreAffinityMask = (static_cast<DWORD_PTR>(1ULL) << processorCount) - 1;
    }
}

void InitializeHardwareSnapshot() {
    g_CpuName = GetCPUName();
    g_GpuName = GetGPUName();
    g_CpuNameW = AnsiToWide(g_CpuName);
    g_GpuNameW = AnsiToWide(g_GpuName);
    g_MemSpeed = GetMemorySpeedSafe();
    DetectHeterogeneousCores();
    if (!g_HasHybridCpu) {
        for (auto& profile : g_Settings.profiles) {
            profile.disableEcores = false;
        }
    }
}

void InitializeHardwareSnapshotAsync() {
    thread([]() {
        InitializeHardwareSnapshot();
        RequestStateSync();
    }).detach();
}

// 单个目标的压制规格
struct TargetSpec {
    wstring name;
    DWORD_PTR affinityMask;
    DWORD priorityClass;
};

// 单快照批量压制：只创建一次进程快照，一次遍历匹配所有目标进程。
// 相比逐个 SuppressProcessTarget（每个目标一次全系统快照），将 O(N) 次快照
// 降为 O(1)，是守护循环里最大的性能优化点。
void ApplyProcessTargets(const vector<TargetSpec>& targets) {
    if (targets.empty()) {
        return;
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }

    PROCESSENTRY32W processEntry = {};
    processEntry.dwSize = sizeof(processEntry);

    if (Process32FirstW(snapshot, &processEntry)) {
        do {
            for (const auto& target : targets) {
                if (_wcsicmp(target.name.c_str(), processEntry.szExeFile) != 0) {
                    continue;
                }

                const DWORD pid = processEntry.th32ProcessID;

                // 用一个句柄同时完成备份与设置，避免重复 OpenProcess
                HANDLE process = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, FALSE, pid);
                if (!process) {
                    process = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
                }
                if (!process) {
                    continue;
                }

                {
                    lock_guard<mutex> lock(g_ProcessBackupMutex);
                    if (g_ProcessBackup.find(pid) == g_ProcessBackup.end()) {
                        DWORD_PTR processMask = target.affinityMask;
                        DWORD_PTR systemMask = target.affinityMask;
                        DWORD oldPriority = target.priorityClass;

                        const DWORD queriedPriority = GetPriorityClass(process);
                        if (queriedPriority != 0) {
                            oldPriority = queriedPriority;
                        }

                        if (!GetProcessAffinityMask(process, &processMask, &systemMask)) {
                            processMask = systemMask != 0 ? systemMask : target.affinityMask;
                        }

                        g_ProcessBackup[pid] = {processMask, oldPriority};
                    }
                }

                SetProcessAffinityMask(process, target.affinityMask);
                SetPriorityClass(process, target.priorityClass);
                CloseHandle(process);
            }
        } while (Process32NextW(snapshot, &processEntry));
    }

    CloseHandle(snapshot);
}

void RestoreProcessPriorityNormal() {
    unordered_map<DWORD, ProcessBackupInfo> backupsCopy;
    {
        lock_guard<mutex> lock(g_ProcessBackupMutex);
        backupsCopy = g_ProcessBackup;
    }

    for (const auto& item : backupsCopy) {
        HANDLE process = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, FALSE, item.first);
        if (!process) {
            continue;
        }
        SetPriorityClass(process, item.second.priorityClass);
        SetProcessAffinityMask(process, item.second.affinityMask);
        CloseHandle(process);
    }

    SYSTEM_INFO systemInfo = {};
    GetSystemInfo(&systemInfo);
    const wchar_t* targetNames[] = {
        L"browser.exe", L"WeGame.exe", L"wegame.exe", L"wegame_env.exe",
        L"steam.exe", L"steamwebhelper.exe", L"msedge.exe", L"cs2.exe",
        L"VALORANT-Win64-Shipping.exe", L"DeltaForce.exe", L"TslGame.exe",
        L"SGuard32.exe", L"SGuard64.exe", L"SGuardSvc32.exe", L"SGuardSvc64.exe",
        L"TenSafe.exe", L"TenSafe64.exe", L"TP3Helper.exe"
    };

    vector<TargetSpec> targets;
    targets.reserve(18);
    for (const auto* target : targetNames) {
        targets.push_back({target, systemInfo.dwActiveProcessorMask, NORMAL_PRIORITY_CLASS});
    }
    ApplyProcessTargets(targets);

    lock_guard<mutex> lock(g_ProcessBackupMutex);
    g_ProcessBackup.clear();
}

DWORD ReadRegDWORD(HKEY rootKey, LPCWSTR subKey, LPCWSTR valueName, DWORD defaultValue) {
    HKEY key = nullptr;
    DWORD value = defaultValue;
    DWORD dataSize = sizeof(value);
    if (RegOpenKeyExW(rootKey, subKey, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        RegQueryValueExW(key, valueName, nullptr, nullptr, reinterpret_cast<LPBYTE>(&value), &dataSize);
        RegCloseKey(key);
    }
    return value;
}

bool SetRegDWORD(HKEY rootKey, LPCWSTR subKey, LPCWSTR valueName, DWORD data) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(rootKey, subKey, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(key, valueName, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&data), sizeof(data));
        RegCloseKey(key);
        return true;
    }
    return false;
}

void BackupEnvironment() {
    g_Backup.vbsState = ReadRegDWORD(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity", L"Enabled", 1);
    g_Backup.hwSchMode = ReadRegDWORD(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"HwSchMode", 1);

    const wstring basePath = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\";
    for (int i = 0; i < 5; ++i) {
        const wstring subKey = basePath + to_wstring(i);
        g_Backup.vsyncStateNvidia[i] = ReadRegDWORD(HKEY_LOCAL_MACHINE, subKey.c_str(), L"VSyncMode", 1);
    }

    // GameDVR 后台录制
    g_Backup.gameDvrEnabled = ReadRegDWORD(HKEY_CURRENT_USER, L"System\\GameConfigStore", L"GameDVR_Enabled", 1);
    g_Backup.appCaptureEnabled = ReadRegDWORD(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\GameDVR", L"AppCaptureEnabled", 1);
    // 系统游戏模式
    g_Backup.gameModeEnabled = ReadRegDWORD(HKEY_CURRENT_USER, L"Software\\Microsoft\\GameBar", L"AutoGameModeEnabled", 1);
    // QoS 带宽预留（0xFFFFFFFF 表示未限制）
    g_Backup.qosNonBestEffort = ReadRegDWORD(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\Psched", L"NonBestEffortLimit", 0xFFFFFFFF);
    // 系统动画开关
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &g_Backup.animClientArea, 0);
    SystemParametersInfoW(SPI_GETMENUANIMATION, 0, &g_Backup.animMenu, 0);
    SystemParametersInfoW(SPI_GETCOMBOBOXANIMATION, 0, &g_Backup.animCombo, 0);
    SystemParametersInfoW(SPI_GETLISTBOXSMOOTHSCROLLING, 0, &g_Backup.animListSmooth, 0);
    SystemParametersInfoW(SPI_GETTOOLTIPANIMATION, 0, &g_Backup.animTooltip, 0);
    SystemParametersInfoW(SPI_GETTOOLTIPFADE, 0, &g_Backup.animTooltipFade, 0);
}

void RestoreEnvironment() {
    if (g_Settings.sysPower) {
        WinExec("powercfg /setactive 381b4222-f694-41d0-9685-e5dace4ba3ed", SW_HIDE);
    }
    if (g_Settings.sysVbs) {
        SetRegDWORD(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity", L"Enabled", g_Backup.vbsState);
    }
    SetRegDWORD(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"HwSchMode", g_Backup.hwSchMode);

    if (g_Settings.sysVsync && g_GpuName.find("NVIDIA") != string::npos) {
        const wstring basePath = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\";
        for (int i = 0; i < 5; ++i) {
            const wstring subKey = basePath + to_wstring(i);
            SetRegDWORD(HKEY_LOCAL_MACHINE, subKey.c_str(), L"VSyncMode", g_Backup.vsyncStateNvidia[i]);
        }
    }

    if (g_Settings.gameDvr) {
        SetRegDWORD(HKEY_CURRENT_USER, L"System\\GameConfigStore", L"GameDVR_Enabled", g_Backup.gameDvrEnabled);
        SetRegDWORD(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\GameDVR", L"AppCaptureEnabled", g_Backup.appCaptureEnabled);
    }
    if (g_Settings.gameMode) {
        SetRegDWORD(HKEY_CURRENT_USER, L"Software\\Microsoft\\GameBar", L"AutoGameModeEnabled", g_Backup.gameModeEnabled);
    }
    if (g_Settings.qosLimit) {
        SetRegDWORD(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\Psched", L"NonBestEffortLimit", g_Backup.qosNonBestEffort);
    }
    if (g_Settings.timerRes) {
        timeEndPeriod(1);
    }
    if (g_Settings.systemAnim) {
        SystemParametersInfoW(SPI_SETCLIENTAREAANIMATION, 0, reinterpret_cast<PVOID>(static_cast<INT_PTR>(g_Backup.animClientArea)), SPIF_SENDCHANGE);
        SystemParametersInfoW(SPI_SETMENUANIMATION, 0, reinterpret_cast<PVOID>(static_cast<INT_PTR>(g_Backup.animMenu)), SPIF_SENDCHANGE);
        SystemParametersInfoW(SPI_SETCOMBOBOXANIMATION, 0, reinterpret_cast<PVOID>(static_cast<INT_PTR>(g_Backup.animCombo)), SPIF_SENDCHANGE);
        SystemParametersInfoW(SPI_SETLISTBOXSMOOTHSCROLLING, 0, reinterpret_cast<PVOID>(static_cast<INT_PTR>(g_Backup.animListSmooth)), SPIF_SENDCHANGE);
        SystemParametersInfoW(SPI_SETTOOLTIPANIMATION, 0, reinterpret_cast<PVOID>(static_cast<INT_PTR>(g_Backup.animTooltip)), SPIF_SENDCHANGE);
        SystemParametersInfoW(SPI_SETTOOLTIPFADE, 0, reinterpret_cast<PVOID>(static_cast<INT_PTR>(g_Backup.animTooltipFade)), SPIF_SENDCHANGE);
    }
}

void ExecuteOptimizations() {
    BackupEnvironment();

    if (g_Settings.sysPower) {
        WinExec("powercfg /setactive e9a42b02-d5df-448d-aa00-03f14749eb61", SW_HIDE);
    }
    if (g_Settings.sysVbs) {
        SetRegDWORD(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity", L"Enabled", 0);
    }
    SetRegDWORD(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"HwSchMode", 2);

    if (g_CpuName.find("Intel") == string::npos && g_Settings.disableHpet) {
        WinExec("bcdedit /deletevalue useplatformclock", SW_HIDE);
    }

    // GameDVR 后台录制（降低录制与后台开销）
    if (g_Settings.gameDvr) {
        SetRegDWORD(HKEY_CURRENT_USER, L"System\\GameConfigStore", L"GameDVR_Enabled", 0);
        SetRegDWORD(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\GameDVR", L"AppCaptureEnabled", 0);
    }
    // 系统游戏模式
    if (g_Settings.gameMode) {
        SetRegDWORD(HKEY_CURRENT_USER, L"Software\\Microsoft\\GameBar", L"AutoGameModeEnabled", 1);
    }
    // 禁用 QoS 带宽预留（需管理员权限）
    if (g_Settings.qosLimit) {
        SetRegDWORD(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\Psched", L"NonBestEffortLimit", 0);
    }
    // 高精度计时器（1ms）
    if (g_Settings.timerRes) {
        timeBeginPeriod(1);
    }
    // 关闭系统动画（减少视觉开销）
    if (g_Settings.systemAnim) {
        SystemParametersInfoW(SPI_SETCLIENTAREAANIMATION, 0, reinterpret_cast<PVOID>(static_cast<INT_PTR>(FALSE)), SPIF_SENDCHANGE);
        SystemParametersInfoW(SPI_SETMENUANIMATION, 0, reinterpret_cast<PVOID>(static_cast<INT_PTR>(FALSE)), SPIF_SENDCHANGE);
        SystemParametersInfoW(SPI_SETCOMBOBOXANIMATION, 0, reinterpret_cast<PVOID>(static_cast<INT_PTR>(FALSE)), SPIF_SENDCHANGE);
        SystemParametersInfoW(SPI_SETLISTBOXSMOOTHSCROLLING, 0, reinterpret_cast<PVOID>(static_cast<INT_PTR>(FALSE)), SPIF_SENDCHANGE);
        SystemParametersInfoW(SPI_SETTOOLTIPANIMATION, 0, reinterpret_cast<PVOID>(static_cast<INT_PTR>(FALSE)), SPIF_SENDCHANGE);
        SystemParametersInfoW(SPI_SETTOOLTIPFADE, 0, reinterpret_cast<PVOID>(static_cast<INT_PTR>(FALSE)), SPIF_SENDCHANGE);
    }

    // 备份已持久化到磁盘：若进程异常退出（崩溃/强杀），下次启动自动恢复
    SaveEnvironmentBackupToFile();
}

void HeartbeatThreadBody(unsigned long generation) {
    SYSTEM_INFO systemInfo = {};
    GetSystemInfo(&systemInfo);

    const DWORD_PTR lastCoreMask = GetHighestSetBitMask(systemInfo.dwActiveProcessorMask);
    DWORD_PTR gameAvoidMask = systemInfo.dwActiveProcessorMask & ~lastCoreMask;
    if (gameAvoidMask == 0) {
        gameAvoidMask = systemInfo.dwActiveProcessorMask;
    }

    const wstring aceList[] = {
        L"SGuard32.exe", L"SGuard64.exe",
        L"SGuardSvc32.exe", L"SGuardSvc64.exe",
        L"TenSafe.exe", L"TenSafe64.exe",
        L"TP3Helper.exe"
    };

    int secondsPassed = 0;

    while (g_IsRunning.load() && generation == g_RunGeneration.load()) {
        const GameProfile& profile = g_Settings.profiles[g_SelectedGame];
        // 读取当前扫描间隔（用户可调），平台扫描自动错开 1 秒以对齐颗粒度
        const int gameInterval = profile.scanInterval;
        const int platformInterval = profile.scanInterval + 1;

        vector<TargetSpec> targets;
        targets.reserve(40);

        // ── 平台周期：Edge / WeGame / ACE / Steam ──
        if (secondsPassed % platformInterval == 0) {
            if (g_Settings.edgeSuppress) {
                targets.push_back({L"msedge.exe", lastCoreMask, IDLE_PRIORITY_CLASS});
            }

            if (IsTencentGame(g_SelectedGame)) {
                if (profile.aceSuppress) {
                    for (const auto& aceProcess : aceList) {
                        targets.push_back({aceProcess, lastCoreMask, IDLE_PRIORITY_CLASS});
                    }
                }
                if (profile.weGameSuppress) {
                    targets.push_back({L"browser.exe", lastCoreMask, IDLE_PRIORITY_CLASS});
                    targets.push_back({L"WeGame.exe", lastCoreMask, IDLE_PRIORITY_CLASS});
                    targets.push_back({L"wegame.exe", lastCoreMask, IDLE_PRIORITY_CLASS});
                    targets.push_back({L"wegame_env.exe", lastCoreMask, IDLE_PRIORITY_CLASS});
                }
            } else if (profile.steamSuppress) {
                targets.push_back({L"steam.exe", lastCoreMask, BELOW_NORMAL_PRIORITY_CLASS});
                targets.push_back({L"steamwebhelper.exe", lastCoreMask, IDLE_PRIORITY_CLASS});
            }
        }

        // ── 游戏周期：游戏进程 + 自定义进程 ──
        if (secondsPassed % gameInterval == 0) {
            // 游戏进程核心掩码：开启「避开最后一核」时排除末核，否则使用全部核心
            const DWORD_PTR gameFullMask = systemInfo.dwActiveProcessorMask;
            const DWORD_PTR gameCoreMask = profile.avoidLastCore ? gameAvoidMask : gameFullMask;

            if (g_HasHybridCpu && profile.disableEcores && g_PCoreAffinityMask != 0) {
                if (g_SelectedGame == 0) targets.push_back({L"VALORANT-Win64-Shipping.exe", g_PCoreAffinityMask, HIGH_PRIORITY_CLASS});
                if (g_SelectedGame == 1) targets.push_back({L"DeltaForce.exe", g_PCoreAffinityMask, HIGH_PRIORITY_CLASS});
                if (g_SelectedGame == 2) targets.push_back({L"cs2.exe", g_PCoreAffinityMask, HIGH_PRIORITY_CLASS});
                if (g_SelectedGame == 3) targets.push_back({L"TslGame.exe", g_PCoreAffinityMask, HIGH_PRIORITY_CLASS});
            }

            if (IsTencentGame(g_SelectedGame)) {
                if (profile.gameAvoid) {
                    if (g_SelectedGame == 0) targets.push_back({L"VALORANT-Win64-Shipping.exe", gameCoreMask, HIGH_PRIORITY_CLASS});
                    if (g_SelectedGame == 1) targets.push_back({L"DeltaForce.exe", gameCoreMask, HIGH_PRIORITY_CLASS});
                }
            } else if (profile.gameFullCore) {
                if (g_SelectedGame == 2) targets.push_back({L"cs2.exe", gameCoreMask, HIGH_PRIORITY_CLASS});
                if (g_SelectedGame == 3) targets.push_back({L"TslGame.exe", gameCoreMask, HIGH_PRIORITY_CLASS});
            }

            // 自定义进程压制列表
            {
                lock_guard<mutex> lock(g_CustomTargetsMutex);
                for (const auto& target : g_Settings.customTargets) {
                    if (target.name.empty()) {
                        continue;
                    }
                    DWORD priorityClass = IDLE_PRIORITY_CLASS;
                    if (target.priority == 1) {
                        priorityClass = BELOW_NORMAL_PRIORITY_CLASS;
                    } else if (target.priority == 2) {
                        priorityClass = NORMAL_PRIORITY_CLASS;
                    }
                    targets.push_back({target.name, lastCoreMask, priorityClass});
                }
            }
        }

        // 单次快照批量应用所有目标
        if (!targets.empty()) {
            ApplyProcessTargets(targets);
        }

        if (WaitForSingleObject(g_HeartbeatStopEvent, 1000) != WAIT_TIMEOUT) {
            break;
        }
        ++secondsPassed;
    }
}

void StartOptimizationGuard() {
    if (g_IsRunning.load()) {
        return;
    }

    if (g_HeartbeatThread.joinable()) {
        g_HeartbeatThread.join();
    }

    ResetEvent(g_HeartbeatStopEvent);
    ExecuteOptimizations();
    g_IsRunning = true;
    const unsigned long generation = g_RunGeneration.fetch_add(1) + 1;
    const int interval = g_Settings.profiles[g_SelectedGame].scanInterval;
    wchar_t statusBuf[128];
    swprintf_s(statusBuf, L"守护已启动。游戏进程每 %d 秒检查一次，平台进程每 %d 秒检查一次。",
               interval, interval + 1);
    g_StatusMessage = statusBuf;
    g_HeartbeatThread = thread([generation]() { HeartbeatThreadBody(generation); });
    RequestStateSync();
}

void StopOptimizationGuard() {
    if (!g_IsRunning.load()) {
        return;
    }

    g_IsRunning = false;
    g_RunGeneration.fetch_add(1);
    SetEvent(g_HeartbeatStopEvent);

    if (g_HeartbeatThread.joinable()) {
        g_HeartbeatThread.join();
    }

    RestoreEnvironment();
    RestoreProcessPriorityNormal();
    g_StatusMessage = L"守护已停止，原始系统设置已恢复。";
    // 正常停止：删除磁盘上的环境备份（下次启动不会再触发恢复）
    if (!g_BackupPath.empty()) {
        DeleteFileW(g_BackupPath.c_str());
    }
    RequestStateSync();
}

vector<wstring> SplitMessage(const wstring& message, wchar_t delimiter) {
    vector<wstring> parts;
    wstringstream stream(message);
    wstring part;
    while (getline(stream, part, delimiter)) {
        parts.push_back(part);
    }
    return parts;
}

void ApplySetting(const wstring& key, bool enabled) {
    // ── 全局系统优化（步骤 1） ──
    if (key == L"sysPower") g_Settings.sysPower = enabled;
    else if (key == L"sysVbs") g_Settings.sysVbs = enabled;
    else if (key == L"sysVsync") g_Settings.sysVsync = enabled;
    else if (key == L"edgeSuppress") g_Settings.edgeSuppress = enabled;
    else if (key == L"disableHpet" && !IsIntelCpu()) g_Settings.disableHpet = enabled;
    else if (key == L"gameDvr") g_Settings.gameDvr = enabled;
    else if (key == L"gameMode") g_Settings.gameMode = enabled;
    else if (key == L"qosLimit") g_Settings.qosLimit = enabled;
    else if (key == L"timerRes") g_Settings.timerRes = enabled;
    else if (key == L"systemAnim") g_Settings.systemAnim = enabled;
    // ── 每游戏配置（步骤 3） ──
    else {
        GameProfile& profile = g_Settings.profiles[g_SelectedGame];
        if (key == L"aceSuppress") profile.aceSuppress = enabled;
        else if (key == L"weGameSuppress") profile.weGameSuppress = enabled;
        else if (key == L"gameAvoid") profile.gameAvoid = enabled;
        else if (key == L"steamSuppress") profile.steamSuppress = enabled;
        else if (key == L"gameFullCore") profile.gameFullCore = enabled;
        else if (key == L"disableEcores" && g_HasHybridCpu) profile.disableEcores = enabled;
        else if (key == L"avoidLastCore") profile.avoidLastCore = enabled;
    }

    SaveSettingsToFile();
}

void HandleWebMessage(const wstring& message) {
    if (message == L"ready") {
        g_PageReady = true;
        PushStateToWebView();
        return;
    }

    const vector<wstring> parts = SplitMessage(message, L'|');
    if (parts.empty()) {
        return;
    }

    if (parts[0] == L"game" && parts.size() >= 2) {
        const int selection = GameSelectionFromId(parts[1]);
        if (selection >= 0) {
            g_SelectedGame = selection;
            SaveSettingsToFile();
        }
        RequestStateSync();
        return;
    }

    if (parts[0] == L"setting" && parts.size() >= 3) {
        if (parts[1] == L"scanInterval") {
            const int val = _wtoi(parts[2].c_str());
            if (val >= 5 && val <= 300) {
                g_Settings.profiles[g_SelectedGame].scanInterval = val;
                SaveSettingsToFile();
            }
        } else {
            ApplySetting(parts[1], parts[2] == L"1");
        }
        RequestStateSync();
        return;
    }

    if (parts[0] == L"custom" && parts.size() >= 2) {
        if (parts[1] == L"add" && parts.size() >= 4) {
            const wstring name = TrimCopyW(parts[2]);
            int priority = _wtoi(parts[3].c_str());
            if (priority < 0) priority = 0;
            if (priority > 2) priority = 2;
            if (!name.empty() && name.size() <= 64) {
                lock_guard<mutex> lock(g_CustomTargetsMutex);
                bool found = false;
                for (auto& target : g_Settings.customTargets) {
                    if (_wcsicmp(target.name.c_str(), name.c_str()) == 0) {
                        target.priority = priority;
                        found = true;
                        break;
                    }
                }
                if (!found && g_Settings.customTargets.size() < 32) {
                    g_Settings.customTargets.push_back({name, priority});
                }
            }
        } else if (parts[1] == L"remove" && parts.size() >= 3) {
            lock_guard<mutex> lock(g_CustomTargetsMutex);
            for (auto it = g_Settings.customTargets.begin(); it != g_Settings.customTargets.end(); ++it) {
                if (_wcsicmp(it->name.c_str(), parts[2].c_str()) == 0) {
                    g_Settings.customTargets.erase(it);
                    break;
                }
            }
        } else if (parts[1] == L"clear") {
            lock_guard<mutex> lock(g_CustomTargetsMutex);
            g_Settings.customTargets.clear();
        }
        SaveSettingsToFile();
        RequestStateSync();
        return;
    }

    if (parts[0] == L"window" && parts.size() >= 2) {
        HandleWindowCommand(parts[1]);
        return;
    }

    if (parts[0] == L"action" && parts.size() >= 2 && parts[1] == L"execute") {
        if (g_IsRunning.load()) {
            StopOptimizationGuard();
        } else {
            StartOptimizationGuard();
        }
    }
}

void ResizeWebViewToClientArea() {
    if (!g_WebViewController || !g_MainWindow) {
        return;
    }

    RECT bounds = {};
    GetClientRect(g_MainWindow, &bounds);
    g_WebViewController->put_Bounds(bounds);
}

void ShowInlineErrorPage(const wstring& title, const wstring& detail) {
    if (!g_WebView) {
        return;
    }

    wstring html =
        L"<!doctype html><html><head><meta charset=\"utf-8\"><title>LYB 引擎</title></head>"
        L"<body style=\"margin:0;font-family:Segoe UI,Arial,sans-serif;background:#020617;color:#e2e8f0;"
        L"display:flex;align-items:center;justify-content:center;min-height:100vh;\">"
        L"<div style=\"max-width:720px;padding:40px;border:1px solid rgba(148,163,184,.2);border-radius:24px;"
        L"background:#0f172a;box-shadow:0 24px 80px rgba(0,0,0,.45);\">"
        L"<h1 style=\"margin:0 0 12px;font-size:28px;\">";
    html += HtmlEscape(title);
    html += L"</h1><p style=\"margin:0;color:#94a3b8;line-height:1.7;\">";
    html += HtmlEscape(detail);
    html += L"</p></div></body></html>";

    g_WebView->NavigateToString(html.c_str());
}

class RefCounted {
protected:
    ULONG AddRefImpl() {
        return ++refCount_;
    }

    ULONG ReleaseImpl() {
        const ULONG value = --refCount_;
        if (value == 0) {
            delete this;
        }
        return value;
    }

    virtual ~RefCounted() = default;

private:
    atomic<ULONG> refCount_{1};
};

// 挂起完成的回调：TrySuspend 是异步的，成功与否通过此回调通知
class TrySuspendCompletedHandler final : public ICoreWebView2TrySuspendCompletedHandler, private RefCounted {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2TrySuspendCompletedHandler) {
            *object = static_cast<ICoreWebView2TrySuspendCompletedHandler*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return AddRefImpl();
    }

    ULONG STDMETHODCALLTYPE Release() override {
        return ReleaseImpl();
    }

    STDMETHODIMP Invoke(HRESULT errorCode, BOOL isSuccessful) override {
        g_WebViewSuspendPending = false;
        if (FAILED(errorCode) || !isSuccessful) {
            // 挂起失败（例如页面正在播放音频）时保持未挂起状态
            g_WebViewSuspended = false;
            g_WebViewResumeRequested = false;
            return S_OK;
        }
        g_WebViewSuspended = true;
        // 挂起期间用户已恢复窗口：投递到 UI 线程立即恢复，避免页面冻结在后台
        if (g_WebViewResumeRequested.exchange(false)) {
            if (g_MainWindow != nullptr) {
                PostMessageW(g_MainWindow, WM_APP_RESUME_WEBVIEW, 0, 0);
            }
        }
        return S_OK;
    }
};

// 窗口失焦/最小化时挂起 WebView，释放渲染进程内存
void SuspendWebViewIfIdle() {
    if (!g_WebView12 || !g_PageReady || g_WebViewSuspended.load() || g_WebViewSuspendPending.load()) {
        return;
    }

    BOOL isSuspended = FALSE;
    if (SUCCEEDED(g_WebView12->get_IsSuspended(&isSuspended)) && isSuspended) {
        g_WebViewSuspended = true;
        return;
    }

    // WebView 可见时 TrySuspend 会失败（0x8007139F），先隐藏再挂起
    if (g_WebViewController) {
        g_WebViewController->put_IsVisible(FALSE);
    }

    g_WebViewSuspendPending = true;
    TrySuspendCompletedHandler* handler = new TrySuspendCompletedHandler();
    const HRESULT hr = g_WebView12->TrySuspend(handler);
    handler->Release();
    if (FAILED(hr)) {
        g_WebViewSuspendPending = false;
    }
}

// 窗口重新获得焦点/还原时恢复 WebView 渲染
void ResumeWebViewIfSuspended() {
    if (!g_WebView12) {
        return;
    }

    // 挂起请求尚未完成：标记待恢复，由完成回调负责立即恢复
    if (g_WebViewSuspendPending.load()) {
        g_WebViewResumeRequested = true;
        return;
    }

    BOOL isSuspended = FALSE;
    if (SUCCEEDED(g_WebView12->get_IsSuspended(&isSuspended)) && isSuspended) {
        g_WebView12->Resume();
    }
    g_WebViewSuspended = false;

    // 恢复可见性
    if (g_WebViewController) {
        g_WebViewController->put_IsVisible(TRUE);
        ResizeWebViewToClientArea();
    }
}

class WebMessageReceivedHandler final : public ICoreWebView2WebMessageReceivedEventHandler, private RefCounted {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2WebMessageReceivedEventHandler) {
            *object = static_cast<ICoreWebView2WebMessageReceivedEventHandler*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return AddRefImpl();
    }

    ULONG STDMETHODCALLTYPE Release() override {
        return ReleaseImpl();
    }

    STDMETHODIMP Invoke(ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) override {
        LPWSTR rawMessage = nullptr;
        if (SUCCEEDED(args->TryGetWebMessageAsString(&rawMessage)) && rawMessage != nullptr) {
            const wstring message(rawMessage);
            CoTaskMemFree(rawMessage);
            HandleWebMessage(message);
        }
        return S_OK;
    }
};

class NavigationStartingHandler final : public ICoreWebView2NavigationStartingEventHandler, private RefCounted {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2NavigationStartingEventHandler) {
            *object = static_cast<ICoreWebView2NavigationStartingEventHandler*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return AddRefImpl();
    }

    ULONG STDMETHODCALLTYPE Release() override {
        return ReleaseImpl();
    }

    STDMETHODIMP Invoke(ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*) override {
        g_PageReady = false;
        if (g_MainWindow != nullptr) {
            InvalidateRect(g_MainWindow, nullptr, TRUE);
        }
        return S_OK;
    }
};

class ControllerCompletedHandler final : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, private RefCounted {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler) {
            *object = static_cast<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return AddRefImpl();
    }

    ULONG STDMETHODCALLTYPE Release() override {
        return ReleaseImpl();
    }

    STDMETHODIMP Invoke(HRESULT controllerResult, ICoreWebView2Controller* controller) override {
        if (FAILED(controllerResult) || controller == nullptr) {
            MessageBoxW(g_MainWindow, L"创建 WebView2 控制器失败。", L"LYB 引擎", MB_ICONERROR);
            return S_OK;
        }

        g_WebViewController = controller;
        g_WebViewController->get_CoreWebView2(&g_WebView);
        g_WebView->QueryInterface(IID_ICoreWebView2_12, reinterpret_cast<void**>(g_WebView12.GetAddressOf()));
        ResizeWebViewToClientArea();

        static const IID kController2Guid =
            {0xc979903e, 0xd4ca, 0x4228, {0x92, 0xeb, 0x47, 0xee, 0x3f, 0xa9, 0x6e, 0xab}};
        ICoreWebView2Controller2* controller2 = nullptr;
        if (SUCCEEDED(g_WebViewController->QueryInterface(kController2Guid, reinterpret_cast<void**>(&controller2))) && controller2 != nullptr) {
            const COREWEBVIEW2_COLOR transparent = {0, 0, 0, 0};
            controller2->put_DefaultBackgroundColor(transparent);
            controller2->Release();
        }

        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(g_WebView->get_Settings(&settings)) && settings) {
            settings->put_IsStatusBarEnabled(FALSE);
            settings->put_AreDefaultContextMenusEnabled(FALSE);
            settings->put_AreDevToolsEnabled(FALSE);
        }

        ICoreWebView2WebMessageReceivedEventHandler* messageHandler = new WebMessageReceivedHandler();
        g_WebView->add_WebMessageReceived(messageHandler, nullptr);
        messageHandler->Release();

        ICoreWebView2NavigationStartingEventHandler* navigationHandler = new NavigationStartingHandler();
        g_WebView->add_NavigationStarting(navigationHandler, nullptr);
        navigationHandler->Release();

        ComPtr<ICoreWebView2_3> webView3;
        if (FAILED(g_WebView->QueryInterface(IID_ICoreWebView2_3, reinterpret_cast<void**>(webView3.GetAddressOf()))) || !webView3) {
            ShowInlineErrorPage(L"WebView2 运行时版本过旧", L"当前运行时不支持本地虚拟主机映射。");
            return S_OK;
        }

        const wstring indexPath = g_HtmlFolder + L"\\index.html";
        if (GetFileAttributesW(indexPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            ShowInlineErrorPage(L"缺少界面文件", L"请确认 html\\index.html 与可执行文件位于同一目录结构下。");
            return S_OK;
        }

        webView3->SetVirtualHostNameToFolderMapping(
            L"appassets",
            g_HtmlFolder.c_str(),
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        g_WebView->Navigate(L"https://appassets/index.html");
        return S_OK;
    }
};

class EnvironmentCompletedHandler final : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, private RefCounted {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (riid == IID_IUnknown || riid == IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler) {
            *object = static_cast<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return AddRefImpl();
    }

    ULONG STDMETHODCALLTYPE Release() override {
        return ReleaseImpl();
    }

    STDMETHODIMP Invoke(HRESULT result, ICoreWebView2Environment* environment) override {
        if (FAILED(result) || environment == nullptr) {
            MessageBoxW(g_MainWindow, L"初始化 WebView2 失败，请先安装 WebView2 Runtime。", L"LYB 引擎", MB_ICONERROR);
            return S_OK;
        }

        ICoreWebView2CreateCoreWebView2ControllerCompletedHandler* controllerHandler = new ControllerCompletedHandler();
        environment->CreateCoreWebView2Controller(g_MainWindow, controllerHandler);
        controllerHandler->Release();
        return S_OK;
    }
};

void InitializeWebView() {
    g_HtmlFolder = GetModuleDirectory() + L"\\html";
    const wstring userDataFolder = GetModuleDirectory() + L"\\webview2_user_data";

    // 通过官方环境变量注入 Chromium 参数，显著降低内存占用：
    //  - disable-gpu / disable-gpu-compositing：界面是简单静态页，无需 GPU 进程
    //  - renderer-process-limit=1：仅保留单个渲染进程
    //  - js-flags=--max-old-space-size=96：限制渲染进程 V8 堆上限
    //  - disable-extensions / disable-background-networking：关闭无关后台组件
    SetEnvironmentVariableW(
        L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS",
        L"--disable-gpu --disable-gpu-compositing --renderer-process-limit=1 "
        L"--js-flags=--max-old-space-size=96 --disable-extensions "
        L"--disable-background-networking --disable-component-update --no-first-run");

    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* environmentHandler = new EnvironmentCompletedHandler();
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        userDataFolder.c_str(),
        nullptr,
        environmentHandler);
    environmentHandler->Release();

    if (FAILED(hr)) {
        MessageBoxW(g_MainWindow, L"加载 WebView2Loader.dll 失败，请确认它与可执行文件在同一目录。", L"LYB 引擎", MB_ICONERROR);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    UNREFERENCED_PARAMETER(wParam);
    UNREFERENCED_PARAMETER(lParam);

    switch (message) {
        case WM_CREATE:
            g_MainWindow = hwnd;
            g_WindowMaximized = IsZoomed(hwnd) != FALSE;
            InitializeWebView();
            InitializeHardwareSnapshotAsync();
            return 0;

        case WM_NCCALCSIZE:
            return 0;

        case WM_NCHITTEST: {
            const LRESULT hit = HitTestResizeBorder(hwnd, lParam);
            if (hit != HTCLIENT) {
                return hit;
            }
            break;
        }

        case WM_GETMINMAXINFO:
            ApplyWindowMinMaxInfo(hwnd, reinterpret_cast<MINMAXINFO*>(lParam));
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            if (!g_PageReady) {
                PAINTSTRUCT paintStruct = {};
                HDC hdc = BeginPaint(hwnd, &paintStruct);
                RECT clientRect = {};
                GetClientRect(hwnd, &clientRect);
                PaintStartupPlaceholder(hdc, clientRect);
                EndPaint(hwnd, &paintStruct);
                return 0;
            }
            break;

        case WM_KILLFOCUS:
            // 失焦不再挂起：只保留最小化时挂起，避免点击其他窗口时页面被隐藏/冻结
            break;

        case WM_SETFOCUS:
            ResumeWebViewIfSuspended();
            break;

        case WM_SIZE:
            ResizeWebViewToClientArea();
            ApplyWindowCornerRegion();
            UpdateWindowStateSnapshot();
            if (wParam == SIZE_MINIMIZED) {
                SuspendWebViewIfIdle();
            } else if (wParam == SIZE_RESTORED || wParam == SIZE_MAXIMIZED) {
                ResumeWebViewIfSuspended();
            }
            return 0;

        case WM_DPICHANGED: {
            const RECT* suggestedRect = reinterpret_cast<RECT*>(lParam);
            if (suggestedRect != nullptr) {
                SetWindowPos(
                    hwnd,
                    nullptr,
                    suggestedRect->left,
                    suggestedRect->top,
                    suggestedRect->right - suggestedRect->left,
                    suggestedRect->bottom - suggestedRect->top,
                    SWP_NOZORDER | SWP_NOACTIVATE);
            }
            ResizeWebViewToClientArea();
            ApplyWindowCornerRegion();
            UpdateWindowStateSnapshot(true);
            return 0;
        }

        case WM_APP_SYNC_STATE:
            PushStateToWebView();
            return 0;

        case WM_APP_RESUME_WEBVIEW:
            ResumeWebViewIfSuspended();
            return 0;

        case WM_APP_TRAYICON:
            if (lParam == WM_LBUTTONDBLCLK || lParam == WM_LBUTTONUP) {
                // 单击/双击托盘图标：显示主窗口
                ShowWindow(hwnd, SW_RESTORE);
                ShowWindow(hwnd, SW_SHOW);
                SetForegroundWindow(hwnd);
                return 0;
            }
            if (lParam == WM_RBUTTONUP) {
                ShowTrayMenu();
                return 0;
            }
            break;

        case WM_CLOSE:
            // 未明确要求退出时，隐藏到托盘继续驻留（守护不中断）
            if (!g_ExitRequested) {
                SaveSettingsToFile();
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            }
            break;

        case WM_DESTROY:
            RemoveTrayIcon();
            StopOptimizationGuard();
            if (g_HeartbeatStopEvent != nullptr) {
                CloseHandle(g_HeartbeatStopEvent);
                g_HeartbeatStopEvent = nullptr;
            }
            g_WebView = nullptr;
            g_WebView12 = nullptr;
            g_WebViewController = nullptr;
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand) {
    EnablePerMonitorDpiAwareness();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    EnableDebugPrivilege();

    // 配置文件与环境备份路径（与 exe 同目录）
    g_ConfigPath = GetModuleDirectory() + L"\\lyb_config.ini";
    g_BackupPath = GetModuleDirectory() + L"\\lyb_env_backup.ini";

    // 上次若异常退出（崩溃/被强杀），残留的环境备份会在此时自动恢复系统设置
    const bool recoveredFromCrash = RecoverEnvironmentFromCrash();

    // 加载上次保存的用户配置（方案、开关、自定义进程、扫描间隔等）
    LoadSettingsFromFile();

    // 管理员权限检查：QoS 带宽预留、VBS 等系统级优化需要管理员权限，
    // 检测到非管理员时提示用户以管理员身份重新启动
    BOOL isAdmin = FALSE;
    {
        SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
        PSID adminGroup = nullptr;
        if (AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
            CheckTokenMembership(nullptr, adminGroup, &isAdmin);
            FreeSid(adminGroup);
        }
    }
    if (!isAdmin) {
        const int choice = MessageBoxW(
            nullptr,
            L"检测到当前未以管理员身份运行。\n\n"
            L"以下系统级优化将无法生效：\n"
            L"  · 关闭 VBS（虚拟化安全）\n"
            L"  · 禁用 QoS 带宽预留\n"
            L"  · 关闭 GameDVR（部分需要写系统配置）\n\n"
            L"是否以管理员身份重新启动？",
            L"LYB 引擎",
            MB_ICONINFORMATION | MB_YESNO);
        if (choice == IDYES) {
            wchar_t exePath[MAX_PATH] = {0};
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            ShellExecuteW(nullptr, L"runas", exePath, nullptr, nullptr, SW_SHOWNORMAL);
            CoUninitialize();
            return 0;
        }
    }

    g_HeartbeatStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = L"LYB_ENGINE_WEBVIEW";
    RegisterClassW(&windowClass);

    RECT workArea = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const LONG workWidth = workArea.right - workArea.left;
    const LONG workHeight = workArea.bottom - workArea.top;
    const LONG initialWidth = max<LONG>(kMinimumWindowWidth, min<LONG>(1560, workWidth - 96));
    const LONG initialHeight = max<LONG>(kMinimumWindowHeight, min<LONG>(1100, workHeight - 96));
    const LONG initialX = workArea.left + max<LONG>(0, (workWidth - initialWidth) / 2);
    const LONG initialY = workArea.top + max<LONG>(0, (workHeight - initialHeight) / 2);

    HWND window = CreateWindowExW(
        WS_EX_APPWINDOW,
        windowClass.lpszClassName,
        L"LYB 引擎 2.0",
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
        initialX,
        initialY,
        initialWidth,
        initialHeight,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (!window) {
        CoUninitialize();
        return 0;
    }

    g_MainWindow = window;
    AddTrayIcon(window);
    if (recoveredFromCrash) {
        g_StatusMessage = L"检测到上次异常退出，已自动恢复原始系统设置。";
    }
    ApplyWindowCornerRegion();
    ShowWindow(window, showCommand);
    UpdateWindow(window);

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    CoUninitialize();
    return static_cast<int>(message.wParam);
}
