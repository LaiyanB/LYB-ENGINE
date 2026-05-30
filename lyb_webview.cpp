#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <windowsx.h>
#include <wrl.h>

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
constexpr LONG kMinimumWindowWidth = 960;
constexpr LONG kMinimumWindowHeight = 680;
constexpr int kWindowCornerRadiusDip = 30;

struct AppSettings {
    bool sysPower = true;
    bool sysVbs = true;
    bool sysVsync = true;
    bool edgeSuppress = true;
    bool disableHpet = true;
    bool aceSuppress = true;
    bool weGameSuppress = true;
    bool gameAvoid = true;
    bool steamSuppress = true;
    bool gameFullCore = true;
    bool disableEcores = false;
    int scanInterval = 60;
} g_Settings;

struct EnvBackup {
    DWORD vbsState = 1;
    DWORD hwSchMode = 1;
    DWORD vsyncStateNvidia[5] = {1, 1, 1, 1, 1};
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
bool g_PageReady = false;
bool g_WindowMaximized = false;

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

void RequestStateSync();

string TrimCopy(const string& value) {
    const size_t start = value.find_first_not_of(" \t\r\n");
    if (start == string::npos) {
        return "";
    }

    const size_t end = value.find_last_not_of(" \t\r\n");
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
    json += L",\"aceSuppress\":";
    json += BoolJson(g_Settings.aceSuppress);
    json += L",\"weGameSuppress\":";
    json += BoolJson(g_Settings.weGameSuppress);
    json += L",\"gameAvoid\":";
    json += BoolJson(g_Settings.gameAvoid);
    json += L",\"steamSuppress\":";
    json += BoolJson(g_Settings.steamSuppress);
    json += L",\"gameFullCore\":";
    json += BoolJson(g_Settings.gameFullCore);
    json += L",\"disableEcores\":";
    json += BoolJson(g_HasHybridCpu && g_Settings.disableEcores);
    json += L",\"scanInterval\":";
    json += to_wstring(g_Settings.scanInterval);
    json += L"}}}";
    return json;
}

void PushStateToWebView() {
    if (!g_PageReady || !g_WebView) {
        return;
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
        g_Settings.disableEcores = false;
    }
}

void InitializeHardwareSnapshotAsync() {
    thread([]() {
        InitializeHardwareSnapshot();
        RequestStateSync();
    }).detach();
}

void SuppressProcessTarget(const wstring& targetName, DWORD_PTR affinityMask, DWORD priorityClass) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }

    PROCESSENTRY32W processEntry = {};
    processEntry.dwSize = sizeof(processEntry);

    if (Process32FirstW(snapshot, &processEntry)) {
        do {
            if (_wcsicmp(targetName.c_str(), processEntry.szExeFile) != 0) {
                continue;
            }

            HANDLE process = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, FALSE, processEntry.th32ProcessID);
            if (!process) {
                process = OpenProcess(PROCESS_SET_INFORMATION, FALSE, processEntry.th32ProcessID);
            }
            if (!process) {
                continue;
            }

            {
                lock_guard<mutex> lock(g_ProcessBackupMutex);
                if (g_ProcessBackup.find(processEntry.th32ProcessID) == g_ProcessBackup.end()) {
                    DWORD_PTR processMask = affinityMask;
                    DWORD_PTR systemMask = affinityMask;
                    DWORD oldPriority = priorityClass;

                    HANDLE queryProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, processEntry.th32ProcessID);
                    if (!queryProcess) {
                        queryProcess = process;
                    }

                    const DWORD queriedPriority = GetPriorityClass(queryProcess);
                    if (queriedPriority != 0) {
                        oldPriority = queriedPriority;
                    }

                    if (!GetProcessAffinityMask(queryProcess, &processMask, &systemMask)) {
                        processMask = systemMask != 0 ? systemMask : affinityMask;
                    }

                    if (queryProcess != process) {
                        CloseHandle(queryProcess);
                    }

                    g_ProcessBackup[processEntry.th32ProcessID] = {processMask, oldPriority};
                }
            }

            SetProcessAffinityMask(process, affinityMask);
            SetPriorityClass(process, priorityClass);
            CloseHandle(process);
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
    const wchar_t* targets[] = {
        L"browser.exe", L"WeGame.exe", L"wegame.exe", L"wegame_env.exe",
        L"steam.exe", L"steamwebhelper.exe", L"msedge.exe", L"cs2.exe",
        L"VALORANT-Win64-Shipping.exe", L"DeltaForce.exe", L"TslGame.exe",
        L"SGuard32.exe", L"SGuard64.exe", L"SGuardSvc32.exe", L"SGuardSvc64.exe",
        L"TenSafe.exe", L"TenSafe64.exe", L"TP3Helper.exe"
    };

    for (const auto* target : targets) {
        SuppressProcessTarget(target, systemInfo.dwActiveProcessorMask, NORMAL_PRIORITY_CLASS);
    }

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
        // 读取当前扫描间隔（用户可调），平台扫描自动错开 1 秒以对齐颗粒度
        const int gameInterval = g_Settings.scanInterval;
        const int platformInterval = g_Settings.scanInterval + 1;

        if (IsTencentGame(g_SelectedGame) && g_Settings.weGameSuppress) {
            SuppressProcessTarget(L"browser.exe", lastCoreMask, IDLE_PRIORITY_CLASS);
            SuppressProcessTarget(L"WeGame.exe", lastCoreMask, IDLE_PRIORITY_CLASS);
            SuppressProcessTarget(L"wegame.exe", lastCoreMask, IDLE_PRIORITY_CLASS);
            SuppressProcessTarget(L"wegame_env.exe", lastCoreMask, IDLE_PRIORITY_CLASS);
        }

        if (secondsPassed % platformInterval == 0) {
            if (g_Settings.edgeSuppress) {
                SuppressProcessTarget(L"msedge.exe", lastCoreMask, IDLE_PRIORITY_CLASS);
            }

            if (IsTencentGame(g_SelectedGame)) {
                if (g_Settings.aceSuppress) {
                    for (const auto& aceProcess : aceList) {
                        SuppressProcessTarget(aceProcess, lastCoreMask, IDLE_PRIORITY_CLASS);
                    }
                }
                if (g_Settings.weGameSuppress) {
                    SuppressProcessTarget(L"browser.exe", lastCoreMask, IDLE_PRIORITY_CLASS);
                    SuppressProcessTarget(L"WeGame.exe", lastCoreMask, IDLE_PRIORITY_CLASS);
                    SuppressProcessTarget(L"wegame.exe", lastCoreMask, IDLE_PRIORITY_CLASS);
                    SuppressProcessTarget(L"wegame_env.exe", lastCoreMask, IDLE_PRIORITY_CLASS);
                }
            } else if (g_Settings.steamSuppress) {
                SuppressProcessTarget(L"steam.exe", lastCoreMask, BELOW_NORMAL_PRIORITY_CLASS);
                SuppressProcessTarget(L"steamwebhelper.exe", lastCoreMask, IDLE_PRIORITY_CLASS);
            }
        }

        if (secondsPassed % gameInterval == 0) {
            if (g_HasHybridCpu && g_Settings.disableEcores && g_PCoreAffinityMask != 0) {
                if (g_SelectedGame == 0) SuppressProcessTarget(L"VALORANT-Win64-Shipping.exe", g_PCoreAffinityMask, HIGH_PRIORITY_CLASS);
                if (g_SelectedGame == 1) SuppressProcessTarget(L"DeltaForce.exe", g_PCoreAffinityMask, HIGH_PRIORITY_CLASS);
                if (g_SelectedGame == 2) SuppressProcessTarget(L"cs2.exe", g_PCoreAffinityMask, HIGH_PRIORITY_CLASS);
                if (g_SelectedGame == 3) SuppressProcessTarget(L"TslGame.exe", g_PCoreAffinityMask, HIGH_PRIORITY_CLASS);
            }

            if (IsTencentGame(g_SelectedGame)) {
                if (g_Settings.gameAvoid) {
                    if (g_SelectedGame == 0) SuppressProcessTarget(L"VALORANT-Win64-Shipping.exe", gameAvoidMask, HIGH_PRIORITY_CLASS);
                    if (g_SelectedGame == 1) SuppressProcessTarget(L"DeltaForce.exe", gameAvoidMask, HIGH_PRIORITY_CLASS);
                }
            } else if (g_Settings.gameFullCore) {
                if (g_SelectedGame == 2) SuppressProcessTarget(L"cs2.exe", gameAvoidMask, HIGH_PRIORITY_CLASS);
                if (g_SelectedGame == 3) SuppressProcessTarget(L"TslGame.exe", gameAvoidMask, HIGH_PRIORITY_CLASS);
            }
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
    wchar_t statusBuf[128];
    swprintf_s(statusBuf, L"守护已启动。游戏进程每 %d 秒检查一次，平台进程每 %d 秒检查一次。",
               g_Settings.scanInterval, g_Settings.scanInterval + 1);
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
    if (key == L"sysPower") g_Settings.sysPower = enabled;
    else if (key == L"sysVbs") g_Settings.sysVbs = enabled;
    else if (key == L"sysVsync") g_Settings.sysVsync = enabled;
    else if (key == L"edgeSuppress") g_Settings.edgeSuppress = enabled;
    else if (key == L"disableHpet" && !IsIntelCpu()) g_Settings.disableHpet = enabled;
    else if (key == L"aceSuppress") g_Settings.aceSuppress = enabled;
    else if (key == L"weGameSuppress") g_Settings.weGameSuppress = enabled;
    else if (key == L"gameAvoid") g_Settings.gameAvoid = enabled;
    else if (key == L"steamSuppress") g_Settings.steamSuppress = enabled;
    else if (key == L"gameFullCore") g_Settings.gameFullCore = enabled;
    else if (key == L"disableEcores" && g_HasHybridCpu) g_Settings.disableEcores = enabled;
    else if (key == L"scanInterval") { /* handled in HandleWebMessage */ }
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
        }
        RequestStateSync();
        return;
    }

    if (parts[0] == L"setting" && parts.size() >= 3) {
        if (parts[1] == L"scanInterval") {
            const int val = _wtoi(parts[2].c_str());
            if (val >= 5 && val <= 300) {
                g_Settings.scanInterval = val;
            }
        } else {
            ApplySetting(parts[1], parts[2] == L"1");
        }
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

        case WM_SIZE:
            ResizeWebViewToClientArea();
            ApplyWindowCornerRegion();
            UpdateWindowStateSnapshot();
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

        case WM_DESTROY:
            StopOptimizationGuard();
            if (g_HeartbeatStopEvent != nullptr) {
                CloseHandle(g_HeartbeatStopEvent);
                g_HeartbeatStopEvent = nullptr;
            }
            g_WebView = nullptr;
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
    const LONG initialHeight = max<LONG>(kMinimumWindowHeight, min<LONG>(980, workHeight - 96));
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
