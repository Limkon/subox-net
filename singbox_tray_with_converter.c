/* * singbox_tray_with_converter.c
 * * Refactored version with:
 * 1. Process Crash Monitoring (MonitorThread)
 * 2. Stdout/Stderr Log Monitoring (LogMonitorThread)
 * 3. Robust log buffer parsing
 * 4. Log Viewer Window to display live sing-box output
 *
 * (Modification): Node switching now targets 'route.final'
 * (Modification): (REMOVED) Node management features (Add/Delete/Update)
 * (NEW): Downloads config.json from 'set.ini' (g_configUrl) on startup.
 * (Fix): (REMOVED) WinINet, PowerShell, and bitsadmin downloaders.
 * (NEW): Using 'curl.exe' (assumed to be in the same directory) to download.
 * (Modification): (REMOVED) Node converter menu item and functionality.
 * (Fix): Using absolute path for curl.exe and launching it directly.
 * (Fix): Added -k (insecure) flag to curl to bypass SSL/TLS verification.
 * (Modification): (REMOVED) Icon load failure warning.
 * (Modification): (NEW) Show "Loading config..." tip on tray icon during download.
 * (Fix): Moved LoadSettings() to before tray icon creation to respect g_isIconVisible on startup.
 * (Modification): (REMOVED) Automatic node switching feature.
 * (Modification): (NEW) Config download URL is now read from set.ini [Settings] ConfigUrl.
 */

// 必须在包含任何 Windows 头文件之前定义
#define _WIN32_IE 0x0601
#define __STDC_WANT_LIB_EXT1__ 1
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wininet.h> // 仍然需要 WinINet 的宏定义
#include <commctrl.h>
#include <time.h> // 用于重启冷却

// 直接包含源文件以简化 TCC 编译过程
#include "cJSON.c"

// 为兼容旧版 SDK (如某些 MinGW 版本) 手动添加缺失的宏定义
#ifndef NIF_GUID
#define NIF_GUID 0x00000020
#endif

#ifndef NOTIFYICON_VERSION_4
#define NOTIFYICON_VERSION_4 4
#endif


// 定义一个唯一的 GUID，仅用于程序单实例
// {BFD8A583-662A-4FE3-9784-FAB78A3386A3}
static const GUID APP_GUID = { 0xbfd8a583, 0x662a, 0x4fe3, { 0x97, 0x84, 0xfa, 0xb7, 0x8a, 0x33, 0x86, 0xa3 } };


#define WM_TRAY (WM_USER + 1)
#define WM_SINGBOX_CRASHED (WM_USER + 2)     // 消息：核心进程崩溃
// #define WM_SINGBOX_RECONNECT (WM_USER + 3)   // (已移除) 自动切换
#define WM_LOG_UPDATE (WM_USER + 3)          // 消息：日志线程发送新的日志文本 (值已调整)

#define ID_TRAY_EXIT 1001
#define ID_TRAY_AUTORUN 1002
#define ID_TRAY_SYSTEM_PROXY 1003
// #define ID_TRAY_OPEN_CONVERTER 1004 // (已移除)
#define ID_TRAY_SETTINGS 1005
// #define ID_TRAY_MANAGE_NODES 1006 // (已移除)
#define ID_TRAY_SHOW_CONSOLE 1007 // 新增：显示日志菜单ID
#define ID_TRAY_NODE_BASE 2000

// (移除了所有节点管理窗口控件ID)

// 日志查看器窗口控件ID
#define ID_LOGVIEWER_EDIT 6001

#define ID_GLOBAL_HOTKEY 9001
#define ID_HOTKEY_CTRL 101

// (移除了 IDR_HTML_CONVERTER 和 RT_HTML 的定义)

// 全局变量
NOTIFYICONDATAW nid;
HWND hwnd;
HMENU hMenu, hNodeSubMenu;
HANDLE hMutex = NULL;
PROCESS_INFORMATION pi = {0};
HFONT g_hFont = NULL; // 全局字体句柄

wchar_t** nodeTags = NULL;
int nodeCount = 0;
int nodeCapacity = 0;
wchar_t currentNode[64] = L"";
int httpPort = 0;

const wchar_t* REG_PATH_PROXY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";

// 新增全局变量
BOOL g_isIconVisible = TRUE;
UINT g_hotkeyModifiers = 0;
UINT g_hotkeyVk = 0;
wchar_t g_iniFilePath[MAX_PATH] = {0};
wchar_t g_configUrl[2048] = {0}; // (--- 新增：用于存储配置URL ---)

// --- 重构：新增守护功能全局变量 ---
HANDLE hMonitorThread = NULL;           // 进程崩溃监控线程
HANDLE hLogMonitorThread = NULL;        // 进程日志监控线程
HANDLE hChildStd_OUT_Rd_Global = NULL;  // 核心进程的标准输出管道（读取端）
BOOL g_isExiting = FALSE;               // 标记是否为用户主动退出/切换

// --- 新增：日志窗口句柄 ---
HWND hLogViewerWnd = NULL; // 日志查看器窗口句柄
HFONT hLogFont = NULL;     // 日志窗口等宽字体
// --- 重构结束 ---

// (移除了 MODIFY_NODE_PARAMS 结构体)


// 函数声明
void ShowTrayTip(const wchar_t* title, const wchar_t* message);
void ShowError(const wchar_t* title, const wchar_t* message);
BOOL ReadFileToBuffer(const wchar_t* filename, char** buffer, long* fileSize);
void CleanupDynamicNodes();
BOOL IsWindows8OrGreater();
void LoadSettings();
void SaveSettings();
void ToggleTrayIconVisibility();
LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
void OpenSettingsWindow();
BOOL ParseTags();
int GetHttpInboundPort();
void StartSingBox();
void SwitchNode(const wchar_t* tag);
void SetSystemProxy(BOOL enable);
BOOL IsSystemProxyEnabled();
void SafeReplaceOutbound(const wchar_t* newTag);
void UpdateMenu();
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
void StopSingBox();
void SetAutorun(BOOL enable);
BOOL IsAutorunEnabled();
// (移除了 OpenConverterHtmlFromResource)
char* ConvertLfToCrlf(const char* input);
// (移除了 CreateDefaultConfig)
BOOL DownloadConfig(const wchar_t* url, const wchar_t* savePath); // 新增

// (移除了所有节点管理函数声明)

// --- 重构：新增日志查看器函数声明 ---
void OpenLogViewerWindow();
LRESULT CALLBACK LogViewerWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
// --- 重构结束 ---


// 辅助函数
void ShowTrayTip(const wchar_t* title, const wchar_t* message) {
    if (!g_isIconVisible) return; // (新增) 如果图标隐藏，则不显示提示
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    wcsncpy(nid.szInfoTitle, title, ARRAYSIZE(nid.szInfoTitle) - 1);
    nid.szInfoTitle[ARRAYSIZE(nid.szInfoTitle) - 1] = L'\0';
    wcsncpy(nid.szInfo, message, ARRAYSIZE(nid.szInfo) - 1);
    nid.szInfo[ARRAYSIZE(nid.szInfo) - 1] = L'\0';
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
}

void ShowError(const wchar_t* title, const wchar_t* message) {
    DWORD errorCode = GetLastError();
    wchar_t* sysMsgBuf = NULL;
    wchar_t fullMessage[1024] = {0};
    wcsncpy(fullMessage, message, ARRAYSIZE(fullMessage) - 1);
    fullMessage[ARRAYSIZE(fullMessage) - 1] = L'\0';
    if (errorCode != 0) {
        FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            (LPWSTR)&sysMsgBuf, 0, NULL);
        if (sysMsgBuf) {
            wcsncat(fullMessage, L"\n\n系统错误信息:\n", ARRAYSIZE(fullMessage) - wcslen(fullMessage) - 1);
            wcsncat(fullMessage, sysMsgBuf, ARRAYSIZE(fullMessage) - wcslen(fullMessage) - 1);
            LocalFree(sysMsgBuf);
        }
    }
    MessageBoxW(NULL, fullMessage, title, MB_OK | MB_ICONERROR);
}

BOOL ReadFileToBuffer(const wchar_t* filename, char** buffer, long* fileSize) {
    FILE* f = NULL;
    if (_wfopen_s(&f, filename, L"rb") != 0 || !f) { return FALSE; }
    fseek(f, 0, SEEK_END);
    *fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (*fileSize <= 0) { fclose(f); return FALSE; }
    *buffer = (char*)malloc(*fileSize + 1);
    if (!*buffer) { fclose(f); return FALSE; }
    fread(*buffer, 1, *fileSize, f);
    (*buffer)[*fileSize] = '\0';
    fclose(f);
    return TRUE;
}
void CleanupDynamicNodes() {
    if (nodeTags) {
        for (int i = 0; i < nodeCount; i++) { free(nodeTags[i]); }
        free(nodeTags);
        nodeTags = NULL;
    }
    nodeCount = 0;
    nodeCapacity = 0;
}

BOOL IsWindows8OrGreater() {
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (hKernel32 == NULL) {
        return FALSE;
    }
    FARPROC pFunc = GetProcAddress(hKernel32, "SetProcessMitigationPolicy");
    return (pFunc != NULL);
}

char* ConvertLfToCrlf(const char* input) {
    if (!input) return NULL;

    int lf_count = 0;
    for (const char* p = input; *p; p++) {
        if (*p == '\n' && (p == input || *(p-1) != '\r')) {
            lf_count++;
        }
    }

    if (lf_count == 0) {
        char* output = (char*)malloc(strlen(input) + 1);
        if(output) strcpy(output, input);
        return output;
    }

    size_t new_len = strlen(input) + lf_count;
    char* output = (char*)malloc(new_len + 1);
    if (!output) return NULL;

    char* dest = output;
    for (const char* src = input; *src; src++) {
        if (*src == '\n' && (src == input || *(src-1) != '\r')) {
            *dest++ = '\r';
            *dest++ = '\n';
        } else {
            *dest++ = *src;
        }
    }
    *dest = '\0';

    return output;
}

// 快捷键设置功能函数
void LoadSettings() {
    // (--- 新增：默认URL ---)
    const wchar_t* defaultConfigUrl = L"https://kcoo.cbu.net/share/view/file/a3b48c486a46629f06af19e8431018d3";
    
    g_hotkeyModifiers = GetPrivateProfileIntW(L"Settings", L"Modifiers", 0, g_iniFilePath);
    g_hotkeyVk = GetPrivateProfileIntW(L"Settings", L"VK", 0, g_iniFilePath);
    g_isIconVisible = GetPrivateProfileIntW(L"Settings", L"ShowIcon", 1, g_iniFilePath);
    // (--- 新增：读取URL ---)
    GetPrivateProfileStringW(L"Settings", L"ConfigUrl", defaultConfigUrl, g_configUrl, ARRAYSIZE(g_configUrl), g_iniFilePath);
}

void SaveSettings() {
    wchar_t buffer[16];
    wsprintfW(buffer, L"%u", g_hotkeyModifiers);
    WritePrivateProfileStringW(L"Settings", L"Modifiers", buffer, g_iniFilePath);
    wsprintfW(buffer, L"%u", g_hotkeyVk);
    WritePrivateProfileStringW(L"Settings", L"VK", buffer, g_iniFilePath);
    wsprintfW(buffer, L"%d", g_isIconVisible);
    WritePrivateProfileStringW(L"Settings", L"ShowIcon", buffer, g_iniFilePath);
    // (--- 新增：保存URL ---)
    WritePrivateProfileStringW(L"Settings", L"ConfigUrl", g_configUrl, g_iniFilePath);
}

void ToggleTrayIconVisibility() {
    if (g_isIconVisible) { Shell_NotifyIconW(NIM_DELETE, &nid); }
    else { Shell_NotifyIconW(NIM_ADD, &nid); }
    g_isIconVisible = !g_isIconVisible;
    SaveSettings();
}

UINT HotkeyfToMod(UINT flags) {
    UINT mods = 0;
    if (flags & HOTKEYF_ALT) mods |= MOD_ALT;
    if (flags & HOTKEYF_CONTROL) mods |= MOD_CONTROL;
    if (flags & HOTKEYF_SHIFT) mods |= MOD_SHIFT;
    if (flags & HOTKEYF_EXT) mods |= MOD_WIN;
    return mods;
}

UINT ModToHotkeyf(UINT mods) {
    UINT flags = 0;
    if (mods & MOD_ALT) flags |= HOTKEYF_ALT;
    if (mods & MOD_CONTROL) flags |= HOTKEYF_CONTROL;
    if (mods & MOD_SHIFT) flags |= HOTKEYF_SHIFT;
    if (mods & MOD_WIN) flags |= HOTKEYF_EXT;
    return flags;
}

LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hHotkey, hLabel, hOkBtn, hCancelBtn;
    switch (msg) {
        case WM_CREATE: {
            hLabel = CreateWindowW(L"STATIC", L"显示/隐藏托盘图标快捷键:", WS_CHILD | WS_VISIBLE, 20, 20, 150, 20, hWnd, NULL, NULL, NULL);
            hHotkey = CreateWindowExW(0, HOTKEY_CLASSW, NULL, WS_CHILD | WS_VISIBLE | WS_BORDER, 20, 45, 240, 25, hWnd, (HMENU)ID_HOTKEY_CTRL, NULL, NULL);
            SendMessageW(hHotkey, HKM_SETHOTKEY, MAKEWORD(g_hotkeyVk, ModToHotkeyf(g_hotkeyModifiers)), 0);
            hOkBtn = CreateWindowW(L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 60, 85, 80, 25, hWnd, (HMENU)IDOK, NULL, NULL);
            hCancelBtn = CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE, 160, 85, 80, 25, hWnd, (HMENU)IDCANCEL, NULL, NULL);

            // 应用字体
            SendMessage(hLabel, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessage(hHotkey, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessage(hOkBtn, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            SendMessage(hCancelBtn, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            break;
        }
        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case IDOK: {
                    LRESULT result = SendMessageW(hHotkey, HKM_GETHOTKEY, 0, 0);
                    UINT newVk = LOBYTE(result);
                    UINT newModsFlags = HIBYTE(result);
                    UINT newMods = HotkeyfToMod(newModsFlags);
                    UnregisterHotKey(hwnd, ID_GLOBAL_HOTKEY);
                    if (RegisterHotKey(hwnd, ID_GLOBAL_HOTKEY, newMods, newVk)) {
                        g_hotkeyModifiers = newMods; g_hotkeyVk = newVk;
                        SaveSettings();
                        MessageBoxW(hWnd, L"快捷键设置成功！", L"提示", MB_OK);
                    } else if (newVk != 0 || newMods != 0) {
                        MessageBoxW(hWnd, L"快捷键设置失败，可能已被其他程序占用。", L"错误", MB_OK | MB_ICONERROR);
                        if (g_hotkeyVk != 0 || g_hotkeyModifiers != 0) { RegisterHotKey(hwnd, ID_GLOBAL_HOTKEY, g_hotkeyModifiers, g_hotkeyVk); }
                    } else {
                        g_hotkeyModifiers = 0; g_hotkeyVk = 0;
                        SaveSettings();
                        MessageBoxW(hWnd, L"快捷键已清除。", L"提示", MB_OK);
                    }
                    DestroyWindow(hWnd);
                    break;
                }
                case IDCANCEL: DestroyWindow(hWnd); break;
            }
            break;
        }
        case WM_CLOSE: DestroyWindow(hWnd); break;
        case WM_DESTROY: EnableWindow(hwnd, TRUE); SetForegroundWindow(hwnd); break;
        default: return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

void OpenSettingsWindow() {
    const wchar_t* SETTINGS_CLASS_NAME = L"SingboxSettingsWindowClass";
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = SettingsWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = SETTINGS_CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    if (!GetClassInfoW(wc.hInstance, SETTINGS_CLASS_NAME, &wc)) { RegisterClassW(&wc); }
    HWND hSettingsWnd = CreateWindowExW(WS_EX_DLGMODALFRAME, SETTINGS_CLASS_NAME, L"隐藏图标", WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 300, 160, hwnd, NULL, wc.hInstance, NULL);
    if (hSettingsWnd) {
        EnableWindow(hwnd, FALSE);
        RECT rc, rcOwner;
        GetWindowRect(hSettingsWnd, &rc);
        GetWindowRect(GetDesktopWindow(), &rcOwner);
        SetWindowPos(hSettingsWnd, HWND_TOP, (rcOwner.right - (rc.right - rc.left)) / 2, (rcOwner.bottom - (rc.bottom - rc.top)) / 2, 0, 0, SWP_NOSIZE);
        ShowWindow(hSettingsWnd, SW_SHOW);
        UpdateWindow(hSettingsWnd);
    }
}

// =========================================================================
// (已修改) 解析 config.json 以获取节点列表和当前节点 (读取 route.final)
// =========================================================================
BOOL ParseTags() {
    CleanupDynamicNodes();
    currentNode[0] = L'\0';
    httpPort = 0;
    char* buffer = NULL;
    long size = 0;
    if (!ReadFileToBuffer(L"config.json", &buffer, &size)) {
        return FALSE;
    }
    cJSON* root = cJSON_Parse(buffer);
    if (!root) {
        free(buffer);
        return FALSE;
    }
    cJSON* outbounds = cJSON_GetObjectItem(root, "outbounds");
    cJSON* outbound = NULL;
    cJSON_ArrayForEach(outbound, outbounds) {
        cJSON* tag = cJSON_GetObjectItem(outbound, "tag");
        if (cJSON_IsString(tag) && tag->valuestring) {
            if (nodeCount >= nodeCapacity) {
                int newCapacity = (nodeCapacity == 0) ? 10 : nodeCapacity * 2;
                wchar_t** newTags = (wchar_t**)realloc(nodeTags, newCapacity * sizeof(wchar_t*));
                if (!newTags) {
                    cJSON_Delete(root);
                    free(buffer);
                    CleanupDynamicNodes();
                    return FALSE;
                }
                nodeTags = newTags;
                nodeCapacity = newCapacity;
            }
            const char* utf8_str = tag->valuestring;
            int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8_str, -1, NULL, 0);
            nodeTags[nodeCount] = (wchar_t*)malloc(wideLen * sizeof(wchar_t));
            if (nodeTags[nodeCount]) {
                MultiByteToWideChar(CP_UTF8, 0, utf8_str, -1, nodeTags[nodeCount], wideLen);
                nodeCount++;
            }
        }
    }
    cJSON* route = cJSON_GetObjectItem(root, "route");
    if (route) {
        // (--- 新逻辑 ---)
        // 直接从 route.final 读取当前节点
        cJSON* final_outbound = cJSON_GetObjectItem(route, "final");
        if (cJSON_IsString(final_outbound) && final_outbound->valuestring) {
            MultiByteToWideChar(CP_UTF8, 0, final_outbound->valuestring, -1, currentNode, ARRAYSIZE(currentNode));
        }
    }
    cJSON* inbounds = cJSON_GetObjectItem(root, "inbounds");
    cJSON* inbound = NULL;
    cJSON_ArrayForEach(inbound, inbounds) {
        cJSON* type = cJSON_GetObjectItem(inbound, "type");
        if (cJSON_IsString(type) && strcmp(type->valuestring, "http") == 0) {
            cJSON* listenPort = cJSON_GetObjectItem(inbound, "listen_port");
            if (cJSON_IsNumber(listenPort)) {
                httpPort = listenPort->valueint;
                break;
            }
        }
    }
    cJSON_Delete(root);
    free(buffer);
    return TRUE;
}


int GetHttpInboundPort() {
    return httpPort;
}


// --- 重构：新增守护线程函数 ---

// 监视 sing-box 核心进程是否崩溃的线程函数
DWORD WINAPI MonitorThread(LPVOID lpParam) {
    HANDLE hProcess = (HANDLE)lpParam;
    
    // 阻塞等待，直到 hProcess 进程终止
    WaitForSingleObject(hProcess, INFINITE);

    // 进程终止后，检查 g_isExiting 标志
    // 如果不是用户主动退出（g_isExiting == FALSE），则向主窗口发送崩溃消息
    if (!g_isExiting) {
        PostMessageW(hwnd, WM_SINGBOX_CRASHED, 0, 0);
    }

    return 0;
}

// 监视 sing-box 核心进程日志输出的线程函数
DWORD WINAPI LogMonitorThread(LPVOID lpParam) {
    char readBuf[4096];      // 原始读取缓冲区
    char lineBuf[8192] = {0}; // 拼接缓冲区，处理跨Read的日志行
    DWORD dwRead;
    BOOL bSuccess;
    // static time_t lastLogTriggeredRestart = 0; // (已移除自动切换)
    // const time_t RESTART_COOLDOWN = 60; // (已移除自动切换)
    HANDLE hPipe = (HANDLE)lpParam;

    while (TRUE) {
        // 从管道读取数据
        bSuccess = ReadFile(hPipe, readBuf, sizeof(readBuf) - 1, &dwRead, NULL);
        
        if (!bSuccess || dwRead == 0) {
            // 管道被破坏或关闭 (例如，sing-box 被终止)
            break; // 线程退出
        }

        // 确保缓冲区以NULL结尾
        readBuf[dwRead] = '\0';

        // --- 新增：转发日志到查看器窗口 ---
        if (hLogViewerWnd != NULL && !g_isExiting) {
            int wideLen = MultiByteToWideChar(CP_UTF8, 0, readBuf, -1, NULL, 0);
            if (wideLen > 0) {
                // 为 wchar_t* 分配内存
                wchar_t* pWideBuf = (wchar_t*)malloc(wideLen * sizeof(wchar_t));
                if (pWideBuf) {
                    MultiByteToWideChar(CP_UTF8, 0, readBuf, -1, pWideBuf, wideLen);
                    
                    // 异步发送消息，将内存指针作为lParam传递
                    // 日志窗口的UI线程将负责 free(pWideBuf)
                    if (!PostMessageW(hLogViewerWnd, WM_LOG_UPDATE, 0, (LPARAM)pWideBuf)) {
                        // 如果PostMessage失败（例如窗口正在关闭），我们必须在这里释放内存
                        free(pWideBuf);
                    }
                }
            }
        }
        // --- 新增结束 ---


        // 将新读取的数据附加到行缓冲区
        strncat(lineBuf, readBuf, sizeof(lineBuf) - strlen(lineBuf) - 1);

        // 如果我们正在退出或切换，不要解析日志
        if (g_isExiting) {
            continue;
        }

        // --- 关键词分析 ---
        // (--- 已移除 ---) 自动切换节点的错误检测 (fatal, dial, timeout, refused)
        
        // 我们需要清理缓冲区，只保留最后一行（可能是半行）
        char* last_newline = strrchr(lineBuf, '\n');
        if (last_newline != NULL) {
            // 找到了换行符，只保留换行符之后的内容
            strcpy(lineBuf, last_newline + 1);
        } else if (strlen(lineBuf) > 4096) {
            // 缓冲区已满但没有换行符（异常情况），清空它以防溢出
            lineBuf[0] = '\0';
        }
        // 如果没有换行符且缓冲区未满，则不执行任何操作，等待下一次 ReadFile 拼接
    }
    
    return 0;
}
// --- 重构结束 ---


// --- 重构：修改 StartSingBox ---
void StartSingBox() {
    HANDLE hPipe_Rd_Local = NULL; // 管道读取端（本地）
    HANDLE hPipe_Wr_Local = NULL; // 管道写入端（本地）
    SECURITY_ATTRIBUTES sa;

    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    // 创建用于 stdout/stderr 的管道
    if (!CreatePipe(&hPipe_Rd_Local, &hPipe_Wr_Local, &sa, 0)) {
        ShowError(L"管道创建失败", L"无法为核心程序创建输出管道。");
        return;
    }
    // 确保管道的读取句柄不能被子进程继承
    if (!SetHandleInformation(hPipe_Rd_Local, HANDLE_FLAG_INHERIT, 0)) {
        ShowError(L"管道句柄属性设置失败", L"无法设置输出管道读取句柄的属性。");
        CloseHandle(hPipe_Rd_Local);
        CloseHandle(hPipe_Wr_Local);
        return;
    }

    // 将本地读取句柄保存到全局变量，以便日志线程使用
    hChildStd_OUT_Rd_Global = hPipe_Rd_Local;

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hPipe_Wr_Local;
    si.hStdError = hPipe_Wr_Local;

    wchar_t cmdLine[MAX_PATH];
    wcsncpy(cmdLine, L"sing-box.exe run -c config.json", ARRAYSIZE(cmdLine));
    cmdLine[ARRAYSIZE(cmdLine) - 1] = L'\0';

    if (!CreateProcessW(NULL, cmdLine, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        ShowError(L"核心程序启动失败", L"无法创建 sing-box.exe 进程。");
        ZeroMemory(&pi, sizeof(pi));
        CloseHandle(hChildStd_OUT_Rd_Global); // 清理全局句柄
        hChildStd_OUT_Rd_Global = NULL;
        CloseHandle(hPipe_Wr_Local);
        return;
    }

    // 子进程已继承写入句柄，我们不再需要它
    CloseHandle(hPipe_Wr_Local);

    // 检查核心是否在500ms内立即退出（通常是配置错误）
    if (WaitForSingleObject(pi.hProcess, 500) == WAIT_OBJECT_0) {
        char chBuf[4096] = {0};
        DWORD dwRead = 0;
        wchar_t errorOutput[4096] = L"";

        // 从管道读取初始错误输出
        if (ReadFile(hChildStd_OUT_Rd_Global, chBuf, sizeof(chBuf) - 1, &dwRead, NULL) && dwRead > 0) {
            chBuf[dwRead] = '\0';
            MultiByteToWideChar(CP_UTF8, 0, chBuf, -1, errorOutput, ARRAYSIZE(errorOutput));
        }

        wchar_t fullMessage[8192];
        wsprintfW(fullMessage, L"sing-box.exe 核心程序启动后立即退出。\n\n可能的原因:\n- 配置文件(config.json)格式错误\n- 核心文件损坏或不兼容\n\n核心程序输出:\n%s", errorOutput);
        ShowError(L"核心程序启动失败", fullMessage);
        
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        ZeroMemory(&pi, sizeof(pi));
        
        CloseHandle(hChildStd_OUT_Rd_Global); // 清理管道
        hChildStd_OUT_Rd_Global = NULL;
    } 
    else {
        // --- 进程启动成功，启动监控线程 ---

        // 1. 启动崩溃监控线程
        hMonitorThread = CreateThread(NULL, 0, MonitorThread, pi.hProcess, 0, NULL);
        
        // 2. 启动日志监控线程
        // 我们必须复制管道句柄，因为 LogMonitorThread 会在退出时关闭它
        HANDLE hPipeForLogThread;
        if (DuplicateHandle(GetCurrentProcess(), hChildStd_OUT_Rd_Global,
                           GetCurrentProcess(), &hPipeForLogThread, 0,
                           FALSE, DUPLICATE_SAME_ACCESS))
        {
            hLogMonitorThread = CreateThread(NULL, 0, LogMonitorThread, hPipeForLogThread, 0, NULL);
        }
        // --- 监控启动完毕 ---
    }

    // 注意：我们 *不* 在这里关闭 hChildStd_OUT_Rd_Global
    // 它由 StopSingBox 统一关闭
}
// --- 重构结束 ---

void SwitchNode(const wchar_t* tag) {
    SafeReplaceOutbound(tag);
    wcsncpy(currentNode, tag, ARRAYSIZE(currentNode) - 1);
    currentNode[ARRAYSIZE(currentNode)-1] = L'\0';
    
    // --- 重构：添加退出标志 ---
    g_isExiting = TRUE; // 标记为主动操作，防止监控线程误报
    StopSingBox();
    g_isExiting = FALSE; // 清除标志，准备重启
    // --- 重构结束 ---

    StartSingBox();
    wchar_t message[256];
    wsprintfW(message, L"当前节点: %s", tag);
    ShowTrayTip(L"切换成功", message);
}

void SetSystemProxy(BOOL enable) {
    int port = GetHttpInboundPort();
    if (port == 0 && enable) {
        MessageBoxW(NULL, L"未找到HTTP入站端口，无法设置系统代理。", L"错误", MB_OK | MB_ICONERROR);
        return;
    }

    if (IsWindows8OrGreater()) {
        HKEY hKey;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_PATH_PROXY, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) != ERROR_SUCCESS) {
            ShowError(L"代理设置失败", L"无法打开注册表键。");
            return;
        }

        if (enable) {
            DWORD dwEnable = 1;
            wchar_t proxyServer[64];
            wsprintfW(proxyServer, L"127.0.0.1:%d", port);
            const wchar_t* proxyBypass = L"<local>";
            RegSetValueExW(hKey, L"ProxyEnable", 0, REG_DWORD, (const BYTE*)&dwEnable, sizeof(dwEnable));
            RegSetValueExW(hKey, L"ProxyServer", 0, REG_SZ, (const BYTE*)proxyServer, (wcslen(proxyServer) + 1) * sizeof(wchar_t));
            RegSetValueExW(hKey, L"ProxyOverride", 0, REG_SZ, (const BYTE*)proxyBypass, (wcslen(proxyBypass) + 1) * sizeof(wchar_t));
        } else {
            DWORD dwEnable = 0;
            RegSetValueExW(hKey, L"ProxyEnable", 0, REG_DWORD, (const BYTE*)&dwEnable, sizeof(dwEnable));
        }
        RegCloseKey(hKey);
    } else {
        INTERNET_PER_CONN_OPTION_LISTW list;
        INTERNET_PER_CONN_OPTIONW options[3];
        DWORD dwBufSize = sizeof(list);
        options[0].dwOption = INTERNET_PER_CONN_FLAGS;
        options[1].dwOption = INTERNET_PER_CONN_PROXY_SERVER;
        options[2].dwOption = INTERNET_PER_CONN_PROXY_BYPASS;
        if (enable) {
            wchar_t proxyServer[64];
            wsprintfW(proxyServer, L"127.0.0.1:%d", port);
            options[0].Value.dwValue = PROXY_TYPE_PROXY;
            options[1].Value.pszValue = proxyServer;
            options[2].Value.pszValue = L"<local>";
        } else {
            options[0].Value.dwValue = PROXY_TYPE_DIRECT;
            options[1].Value.pszValue = L"";
            options[2].Value.pszValue = L"";
        }
        list.dwSize = sizeof(list);
        list.pszConnection = NULL;
        list.dwOptionCount = 3;
        list.dwOptionError = 0;
        list.pOptions = options;
        if (!InternetSetOptionW(NULL, INTERNET_OPTION_PER_CONNECTION_OPTION, &list, dwBufSize)) {
            ShowError(L"代理设置失败", L"调用 InternetSetOptionW 失败。");
            return;
        }
    }

    InternetSetOptionW(NULL, INTERNET_OPTION_SETTINGS_CHANGED, NULL, 0);
    InternetSetOptionW(NULL, INTERNET_OPTION_REFRESH, NULL, 0);
}

BOOL IsSystemProxyEnabled() {
    HKEY hKey;
    DWORD dwEnable = 0;
    DWORD dwSize = sizeof(dwEnable);
    wchar_t proxyServer[MAX_PATH] = {0};
    DWORD dwProxySize = sizeof(proxyServer);
    int port = GetHttpInboundPort();
    BOOL isEnabled = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH_PROXY, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hKey, L"ProxyEnable", NULL, NULL, (LPBYTE)&dwEnable, &dwSize) == ERROR_SUCCESS) {
            if (dwEnable == 1) {
                if (port > 0) {
                    wchar_t expectedProxyServer[64];
                    wsprintfW(expectedProxyServer, L"127.0.0.1:%d", port);
                    if (RegQueryValueExW(hKey, L"ProxyServer", NULL, NULL, (LPBYTE)proxyServer, &dwProxySize) == ERROR_SUCCESS) {
                        if (wcscmp(proxyServer, expectedProxyServer) == 0) {
                            isEnabled = TRUE;
                        }
                    }
                }
            }
        }
        RegCloseKey(hKey);
    }
    return isEnabled;
}

// =========================================================================
// (已修改) 安全地修改 config.json 中的路由 (修改 route.final)
// =========================================================================
void SafeReplaceOutbound(const wchar_t* newTag) {
    char* buffer = NULL;
    long size = 0;
    if (!ReadFileToBuffer(L"config.json", &buffer, &size)) {
        MessageBoxW(NULL, L"无法打开 config.json", L"错误", MB_OK | MB_ICONERROR);
        return;
    }
    int mbLen = WideCharToMultiByte(CP_UTF8, 0, newTag, -1, NULL, 0, NULL, NULL);
    char* newTagMb = (char*)malloc(mbLen);
    if (!newTagMb) {
        free(buffer);
        return;
    }
    WideCharToMultiByte(CP_UTF8, 0, newTag, -1, newTagMb, mbLen, NULL, NULL);
    cJSON* root = cJSON_Parse(buffer);
    if (!root) {
        free(buffer);
        free(newTagMb);
        return;
    }

    // (--- 已修改 ---)
    cJSON* route = cJSON_GetObjectItem(root, "route");
    if (route) {
        // (--- 新逻辑 ---)
        // 直接修改 route.final
        cJSON* final_outbound = cJSON_GetObjectItem(route, "final");
        if (final_outbound) {
            cJSON_SetValuestring(final_outbound, newTagMb);
        } else {
            // (--- 备用逻辑 ---)
            // 如果 "final" 字段不存在，则创建它
            cJSON_AddItemToObject(route, "final", cJSON_CreateString(newTagMb));
        }
    }

    char* newContent = cJSON_PrintBuffered(root, 1, 1);

    if (newContent) {
        FILE* out = NULL;
        if (_wfopen_s(&out, L"config.json", L"wb") == 0 && out != NULL) {
            fwrite(newContent, 1, strlen(newContent), out);
            fclose(out);
        }
        free(newContent);
    }
    cJSON_Delete(root);
    free(buffer);
    free(newTagMb);
}
// --- 重构：修改 UpdateMenu (移除管理节点) ---
void UpdateMenu() {
    if (hMenu) DestroyMenu(hMenu);
    if (hNodeSubMenu) DestroyMenu(hNodeSubMenu);
    hMenu = CreatePopupMenu();
    hNodeSubMenu = CreatePopupMenu();
    for (int i = 0; i < nodeCount; ++i) {
        UINT flags = MF_STRING;
        if (wcscmp(nodeTags[i], currentNode) == 0) { flags |= MF_CHECKED; }
        AppendMenuW(hNodeSubMenu, flags, ID_TRAY_NODE_BASE + i, nodeTags[i]);
    }
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hNodeSubMenu, L"切换节点");
    // (移除了 "管理节点" 菜单项)
    // (移除了 "节点转换" 菜单项)
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_AUTORUN, L"开机启动");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SYSTEM_PROXY, L"系统代理");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SETTINGS, L"隐藏图标");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SHOW_CONSOLE, L"显示日志"); // 新增
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"退出");
}
// --- 重构结束 ---


// --- 重构：修改 WndProc (移除自动切换节点, 移除管理节点) ---
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // 自动重启的冷却计时器
    static time_t lastAutoRestart = 0;
    const time_t RESTART_COOLDOWN = 60; // 60秒 (保留定义，用于崩溃提示)

    if (msg == WM_TRAY && (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU)) {
        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(hWnd);
        ParseTags();
        UpdateMenu();
        CheckMenuItem(hMenu, ID_TRAY_AUTORUN, IsAutorunEnabled() ? MF_CHECKED : MF_UNCHECKED);
        CheckMenuItem(hMenu, ID_TRAY_SYSTEM_PROXY, IsSystemProxyEnabled() ? MF_CHECKED : MF_UNCHECKED);
        TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hWnd, NULL);
        PostMessage(hWnd, WM_NULL, 0, 0);
    }
    else if (msg == WM_COMMAND) {
        int id = LOWORD(wParam);
        if (id == ID_TRAY_EXIT) {
            
            g_isExiting = TRUE; // 标记为主动退出

            // --- 新增：销毁日志窗口 ---
            if (hLogViewerWnd != NULL) {
                DestroyWindow(hLogViewerWnd);
            }
            // --- 新增结束 ---

            UnregisterHotKey(hWnd, ID_GLOBAL_HOTKEY);
            if(g_isIconVisible) Shell_NotifyIconW(NIM_DELETE, &nid);
            if (IsSystemProxyEnabled()) SetSystemProxy(FALSE);
            StopSingBox();
            CleanupDynamicNodes();
            PostQuitMessage(0);
        } else if (id == ID_TRAY_AUTORUN) {
            SetAutorun(!IsAutorunEnabled());
        } else if (id == ID_TRAY_SYSTEM_PROXY) {
            BOOL isEnabled = IsSystemProxyEnabled();
            SetSystemProxy(!isEnabled);
            ShowTrayTip(L"系统代理", isEnabled ? L"系统代理已关闭" : L"系统代理已开启");
        } 
        // (移除了 ID_TRAY_OPEN_CONVERTER 的 else if)
        else if (id == ID_TRAY_SETTINGS) {
            OpenSettingsWindow();
        } 
        // (移除了 ID_TRAY_MANAGE_NODES 的 else if)
        else if (id == ID_TRAY_SHOW_CONSOLE) { // --- 新增：处理日志窗口 ---
            OpenLogViewerWindow();
        } else if (id >= ID_TRAY_NODE_BASE && id < ID_TRAY_NODE_BASE + nodeCount) {
            SwitchNode(nodeTags[id - ID_TRAY_NODE_BASE]);
        }
    } else if (msg == WM_HOTKEY) {
        if (wParam == ID_GLOBAL_HOTKEY) {
            ToggleTrayIconVisibility();
        }
    }
    // --- 重构：处理核心崩溃或日志错误 (已移除自动切换) ---
    else if (msg == WM_SINGBOX_CRASHED) {
        // 核心崩溃，只提示，不自动操作
        ShowTrayTip(L"Sing-box 监控", L"核心进程意外终止。请手动检查。");
    }
    // (--- 已移除 WM_SINGBOX_RECONNECT (自动切换) 的处理 ---)
    // --- 重构结束 ---
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
// --- 重构结束 ---
// --- 重构：修改 StopSingBox ---
void StopSingBox() {
    // 标记为正在退出，让监控线程自行终止
    g_isExiting = TRUE; 

    // 1. 停止核心进程
    if (pi.hProcess) {
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        if (exitCode == STILL_ACTIVE) {
            TerminateProcess(pi.hProcess, 0);
            WaitForSingleObject(pi.hProcess, 5000);
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    
    // 2. 终止并清理崩溃监控线程
    if (hMonitorThread) {
        // 进程终止后，此线程会很快退出
        WaitForSingleObject(hMonitorThread, 1000);
        CloseHandle(hMonitorThread);
    }

    // 3. 终止并清理日志监控线程
    if (hChildStd_OUT_Rd_Global) {
        // 关闭管道的读取端，这将导致 LogMonitorThread 中的 ReadFile 失败
        CloseHandle(hChildStd_OUT_Rd_Global);
    }
    if (hLogMonitorThread) {
        // 等待日志线程安全退出
        WaitForSingleObject(hLogMonitorThread, 1000);
        CloseHandle(hLogMonitorThread);
    }

    // 4. 重置所有全局句柄
    ZeroMemory(&pi, sizeof(pi));
    hMonitorThread = NULL;
    hLogMonitorThread = NULL;
    hChildStd_OUT_Rd_Global = NULL;
    
    // g_isExiting 会在 StartSingBox 或程序退出前被重置
}
// --- 重构结束 ---

void SetAutorun(BOOL enable) {
    HKEY hKey;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
    if (hKey) {
        if (enable) {
            RegSetValueExW(hKey, L"singbox_tray", 0, REG_SZ, (BYTE*)path, (wcslen(path) + 1) * sizeof(wchar_t));
        } else {
            RegDeleteValueW(hKey, L"singbox_tray");
        }
        RegCloseKey(hKey);
    }
}

BOOL IsAutorunEnabled() {
    HKEY hKey;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t value[MAX_PATH];
        DWORD size = sizeof(value);
        LONG res = RegQueryValueExW(hKey, L"singbox_tray", NULL, NULL, (LPBYTE)value, &size);
        RegCloseKey(hKey);
        return (res == ERROR_SUCCESS && wcscmp(value, path) == 0);
    }
    return FALSE;
}

// (--- 移除了 OpenConverterHtmlFromResource ---)

// =========================================================================
// (--- 修改 ---) 从网络下载配置文件
// (修正): 切换到 curl.exe (需要 curl.exe 在同一目录)
// (修正): 使用绝对路径启动 curl.exe，并设置工作目录
// =========================================================================
BOOL DownloadConfig(const wchar_t* url, const wchar_t* savePath) {
    wchar_t cmdLine[4096]; // (--- 缓冲区增大以容纳更长的URL ---)
    wchar_t fullSavePath[MAX_PATH];
    wchar_t fullCurlPath[MAX_PATH];
    wchar_t moduleDir[MAX_PATH];

    // 1. 获取程序 .exe 所在的目录
    GetModuleFileNameW(NULL, moduleDir, MAX_PATH);
    wchar_t* p = wcsrchr(moduleDir, L'\\');
    if (p) {
        *p = L'\0'; // 截断文件名，只保留目录
    } else {
        // 无法获取目录，使用当前目录
        wcsncpy(moduleDir, L".", MAX_PATH);
    }

    // 2. 构建 curl.exe 的绝对路径
    wsprintfW(fullCurlPath, L"%s\\curl.exe", moduleDir);

    // 3. 检查 curl.exe 是否真的存在
    DWORD fileAttr = GetFileAttributesW(fullCurlPath);
    if (fileAttr == INVALID_FILE_ATTRIBUTES || (fileAttr & FILE_ATTRIBUTE_DIRECTORY)) {
         wchar_t errorMsg[MAX_PATH + 256];
         wsprintfW(errorMsg, L"启动失败：未找到 curl.exe。\n\n"
                            L"请确保 curl.exe 位于此路径：\n%s",
                            fullCurlPath);
         MessageBoxW(NULL, errorMsg, L"文件缺失", MB_OK | MB_ICONERROR);
        return FALSE;
    }

    // 4. 获取 config.json 的绝对路径
    if (GetFullPathNameW(savePath, MAX_PATH, fullSavePath, NULL) == 0) {
        ShowError(L"下载失败", L"无法获取 config.json 的绝对路径。");
        return FALSE;
    }

    // 5. 构造 curl.exe 命令
    // -k 允许不安全的 SSL 连接 (跳过证书验证)
    // -L 跟随重定向
    // -sS 静默但显示错误
    // -o 输出文件
    wsprintfW(cmdLine, 
        L"\"%s\" -ksSL -o \"%s\" \"%s\"", // 注意：不再需要 cmd.exe /C
        fullCurlPath, fullSavePath, url
    );

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION downloaderPi = {0};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE; // 隐藏 cmd 窗口

    // 6. 直接执行 curl.exe，并将工作目录设置为 .exe 所在目录
    if (!CreateProcessW(NULL,           // lpApplicationName (use cmdLine)
                        cmdLine,        // lpCommandLine (必须是可修改的)
                        NULL,           // lpProcessAttributes
                        NULL,           // lpThreadAttributes
                        FALSE,          // bInheritHandles
                        CREATE_NO_WINDOW, // dwCreationFlags
                        NULL,           // lpEnvironment
                        moduleDir,      // lpCurrentDirectory (在 .exe 所在目录运行)
                        &si,            // lpStartupInfo
                        &downloaderPi)) // lpProcessInformation
    {
        ShowError(L"下载失败", L"无法启动 curl.exe 下载进程 (CreateProcessW)。");
        return FALSE;
    }

    // 7. 等待下载进程完成 (最多30秒)
    DWORD waitResult = WaitForSingleObject(downloaderPi.hProcess, 30000); 

    if (waitResult == WAIT_TIMEOUT) {
        ShowError(L"下载失败", L"curl.exe 下载超时 (30秒)。");
        TerminateProcess(downloaderPi.hProcess, 1);
        CloseHandle(downloaderPi.hProcess);
        CloseHandle(downloaderPi.hThread);
        return FALSE;
    }

    DWORD exitCode = 1;
    GetExitCodeProcess(downloaderPi.hProcess, &exitCode);
    
    CloseHandle(downloaderPi.hProcess);
    CloseHandle(downloaderPi.hThread);

    if (exitCode != 0) {
        ShowError(L"下载失败", L"curl.exe 报告了错误 (退出码非0)。\n请检查网络或 URL 是否正确。");
        return FALSE;
    }

    // 8. 检查文件是否真的被下载了
    long fileSize = 0;
    char* fileBuffer = NULL;
    if (ReadFileToBuffer(savePath, &fileBuffer, &fileSize)) {
        if (fileSize < 50) { // 假设一个有效的 JSON 配置至少大于 50 字节
             ShowError(L"下载失败", L"下载的文件过小 (小于 50 字节)。\n"
                                   L"这可能是一个错误页面，请检查 URL 是否为[原始]链接。");
             free(fileBuffer);
             return FALSE;
        }
        free(fileBuffer);
        // 文件存在且大小不为0，视为成功
        return TRUE; 
    } else {
        ShowError(L"下载失败", L"curl.exe 报告成功，但无法读取下载的 config.json 文件。");
        return FALSE;
    }
}

// =========================================================================
// (--- 移除了所有节点管理功能实现 ---)
// =========================================================================


// =========================================================================
// (--- 新增 ---) 日志查看器功能实现
// =========================================================================

// 日志窗口过程函数
LRESULT CALLBACK LogViewerWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hEdit = NULL;
    // 定义日志缓冲区的大小限制，防止窗口因日志过多而卡死
    const int MAX_LOG_LENGTH = 200000;  // 最大字符数
    const int TRIM_LOG_LENGTH = 100000; // 裁剪后保留的字符数

    switch (msg) {
        case WM_CREATE: {
            hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                    ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                                    0, 0, 0, 0, // 将在 WM_SIZE 中调整大小
                                    hWnd, (HMENU)ID_LOGVIEWER_EDIT,
                                    GetModuleHandle(NULL), NULL);
            
            if (hEdit == NULL) {
                ShowError(L"创建失败", L"无法创建日志显示框。");
                return -1; // 阻止窗口创建
            }
            
            // 设置等宽字体
            SendMessage(hEdit, WM_SETFONT, (WPARAM)hLogFont, TRUE);
            break;
        }

        case WM_LOG_UPDATE: {
            // 这是从 LogMonitorThread 线程接收到的消息
            wchar_t* pLogChunk = (wchar_t*)lParam;
            if (pLogChunk) {
                // 性能优化：检查是否需要裁剪日志
                int textLen = GetWindowTextLengthW(hEdit);
                if (textLen > MAX_LOG_LENGTH) {
                    // 裁剪：删除前 TRIM_LOG_LENGTH 个字符
                    SendMessageW(hEdit, EM_SETSEL, 0, TRIM_LOG_LENGTH);
                    SendMessageW(hEdit, EM_REPLACESEL, 0, (LPARAM)L"[... 日志已裁剪 ...]\r\n");
                }

                // 追加新文本
                SendMessageW(hEdit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1); // 移动到文本末尾
                SendMessageW(hEdit, EM_REPLACESEL, 0, (LPARAM)pLogChunk); // 追加新日志
                
                // 释放由 LogMonitorThread 分配的内存
                free(pLogChunk);
            }
            break;
        }

        case WM_SIZE: {
            // 窗口大小改变时，自动填满 EDIT 控件
            RECT rcClient;
            GetClientRect(hWnd, &rcClient);
            MoveWindow(hEdit, 0, 0, rcClient.right, rcClient.bottom, TRUE);
            break;
        }

        case WM_CLOSE: {
            // 用户点击关闭时，只隐藏窗口，不销毁
            // 这样下次打开时，日志历史还在
            ShowWindow(hWnd, SW_HIDE);
            hLogViewerWnd = NULL; // 标记为“已关闭”
            break;
        }

        case WM_DESTROY: {
            // 窗口被真正销毁时（例如程序退出时）
            hLogViewerWnd = NULL;
            break;
        }

        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

// 打开或显示日志窗口
void OpenLogViewerWindow() {
    if (hLogViewerWnd != NULL) {
        // 窗口已存在，只需显示并置顶
        ShowWindow(hLogViewerWnd, SW_SHOW);
        SetForegroundWindow(hLogViewerWnd);
        return;
    }

    // 窗口不存在，需要创建
    const wchar_t* LOGVIEWER_CLASS_NAME = L"SingboxLogViewerClass";
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = LogViewerWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = LOGVIEWER_CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(GetModuleHandle(NULL), MAKEINTRESOURCE(1)); // 使用主程序图标
    if (wc.hIcon == NULL) {
        wc.hIcon = LoadIconW(NULL, IDI_APPLICATION); // 备用图标
    }

    if (!GetClassInfoW(wc.hInstance, LOGVIEWER_CLASS_NAME, &wc)) {
        if (!RegisterClassW(&wc)) {
            ShowError(L"窗口注册失败", L"无法注册日志窗口类。");
            return;
        }
    }

    hLogViewerWnd = CreateWindowExW(
        0, LOGVIEWER_CLASS_NAME, L"Sing-box 实时日志",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 700, 450,
        hwnd, // 父窗口设为主窗口，以便管理
        NULL, wc.hInstance, NULL
    );

    if (hLogViewerWnd) {
        // 尝试将窗口居中
        RECT rc, rcOwner;
        GetWindowRect(hLogViewerWnd, &rc);
        GetWindowRect(GetDesktopWindow(), &rcOwner);
        SetWindowPos(hLogViewerWnd, HWND_TOP,
            (rcOwner.right - (rc.right - rc.left)) / 2,
            (rcOwner.bottom - (rc.bottom - rc.top)) / 2,
            0, 0, SWP_NOSIZE);
    } else {
        ShowError(L"窗口创建失败", L"无法创建日志窗口。");
    }
}


// =========================================================================
// 主函数
// =========================================================================

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR lpCmdLine, int nCmdShow) {
    wchar_t mutexName[128];
    wchar_t guidString[40];

    g_hFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"宋体");

    // --- 新增：创建日志等宽字体 ---
    hLogFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (hLogFont == NULL) {
        hLogFont = (HFONT)GetStockObject(SYSTEM_FIXED_FONT); // 备用字体
    }
    // --- 新增结束 ---

    wsprintfW(guidString, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        APP_GUID.Data1, APP_GUID.Data2, APP_GUID.Data3,
        (UINT)APP_GUID.Data4[0], (UINT)APP_GUID.Data4[1], (UINT)APP_GUID.Data4[2], (UINT)APP_GUID.Data4[3],
        (UINT)APP_GUID.Data4[4], (UINT)APP_GUID.Data4[5], (UINT)APP_GUID.Data4[6], (UINT)APP_GUID.Data4[7]);

    wsprintfW(mutexName, L"Global\\%s", guidString);

    hMutex = CreateMutexW(NULL, TRUE, mutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"程序已在运行。", L"提示", MB_OK | MB_ICONINFORMATION);
        if (hMutex) CloseHandle(hMutex);
        if (g_hFont) DeleteObject(g_hFont);
        if (hLogFont) DeleteObject(hLogFont); // 退出前清理
        return 0;
    }

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_HOTKEY_CLASS;
    InitCommonControlsEx(&icex);
    wchar_t szPath[MAX_PATH];
    GetModuleFileNameW(NULL, szPath, MAX_PATH);
    wchar_t* p = wcsrchr(szPath, L'\\');
    if (p) {
        *p = L'\0';
        SetCurrentDirectoryW(szPath); // 仍然设置当前目录，用于 set.ini
        wcsncpy(g_iniFilePath, szPath, MAX_PATH - 1);
        g_iniFilePath[MAX_PATH - 1] = L'\0';
        wcsncat(g_iniFilePath, L"\\set.ini", MAX_PATH - wcslen(g_iniFilePath) - 1);
    } else {
        wcsncpy(g_iniFilePath, L"set.ini", MAX_PATH - 1);
    }

    // --- (修改) 启动逻辑 ---
    
    // (--- 新增：提前加载设置 ---)
    // 必须在注册热键和显示托盘图标之前加载
    // (--- g_configUrl 将在此处被加载 ---)
    LoadSettings();

    // 1. 创建窗口和托盘图标
    const wchar_t* CLASS_NAME = L"TrayWindowClass";
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCE(1));
    if (!wc.hIcon) {
        // (移除了图标加载失败的提示)
        wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    }
    RegisterClassW(&wc);
    hwnd = CreateWindowExW(0, CLASS_NAME, L"TrayApp", 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (!hwnd) {
        if (g_hFont) DeleteObject(g_hFont);
        if (hLogFont) DeleteObject(hLogFont); // 退出前清理
        return 1;
    }

    // (--- 移动到 LoadSettings() 之后 ---)
    // 现在 g_hotkeyVk 和 g_hotkeyModifiers 已经从 set.ini 加载
    if (g_hotkeyVk != 0 || g_hotkeyModifiers != 0) {
        if (!RegisterHotKey(hwnd, ID_GLOBAL_HOTKEY, g_hotkeyModifiers, g_hotkeyVk)) {
            MessageBoxW(NULL, L"注册全局快捷键失败！\n可能已被其他程序占用。", L"快捷键错误", MB_OK | MB_ICONWARNING);
        }
    }

    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = wc.hIcon;
    wcsncpy(nid.szTip, L"程序正在启动...", ARRAYSIZE(nid.szTip) - 1);
    nid.szTip[ARRAYSIZE(nid.szTip) - 1] = L'\0';

    // (--- 移动到 LoadSettings() 之后 ---)
    // 现在 g_isIconVisible 已经从 set.ini 加载
    if (g_isIconVisible) {
        Shell_NotifyIconW(NIM_ADD, &nid);
    }

    // 2. (新增) 显示“正在加载”提示
    // (--- 移动到 LoadSettings() 之后 ---)
    // 如果 g_isIconVisible 为 FALSE, ShowTrayTip 内部会直接返回
    ShowTrayTip(L"请稍候", L"正在获取最新配置文件...");

    // 3. (修改) 下载配置文件
    // (--- 移除硬编码URL, g_configUrl 在 LoadSettings() 中加载 ---)
    // const wchar_t* configUrl = L"..."; 
    const wchar_t* configPath = L"config.json";
    
    // (--- 修改：使用 g_configUrl ---)
    // (--- 此处实现了下载失败时拒绝启动的功能 ---)
    if (!DownloadConfig(g_configUrl, configPath)) {
        // 错误消息已在 DownloadConfig 内部显示
        if (hMutex) CloseHandle(hMutex);
        if (g_hFont) DeleteObject(g_hFont);
        if (hLogFont) DeleteObject(hLogFont);
        DestroyWindow(hwnd); // 退出前销毁窗口
        PostQuitMessage(1);  // 退出程序
        return 1;
    }
    // --- 修改结束 ---
    
    // (--- 调试：修改 ParseTags 失败后的提示 ---)
    if (!ParseTags()) {
        MessageBoxW(NULL, L"无法读取或解析下载的 config.json 文件。\n\n请检查：\n1. 下载的文件是否保存在程序目录。\n2. 该文件是否为有效的JSON格式（而不是HTML页面）。", L"JSON 解析失败", MB_OK | MB_ICONERROR);
        if (hMutex) CloseHandle(hMutex);
        if (g_hFont) DeleteObject(g_hFont);
        if (hLogFont) DeleteObject(hLogFont); // 退出前清理
        DestroyWindow(hwnd); // 退出前销毁窗口
        PostQuitMessage(1);  // 退出程序
        return 1;
    }
    
    // (修改) 更新托盘提示为“程序正在运行”
    wcsncpy(nid.szTip, L"程序正在运行...", ARRAYSIZE(nid.szTip) - 1);
    // (修改) 仅当图标可见时才更新提示
    if(g_isIconVisible) {
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    
    // 确保启动前 g_isExiting 为 false
    g_isExiting = FALSE;
    StartSingBox();
    
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        // --- 新增：检查是否是日志窗口的消息 ---
        // IsDialogMessage 允许在日志窗口的 EDIT 控件中使用 TAB 键
        if (hLogViewerWnd == NULL || !IsDialogMessageW(hLogViewerWnd, &msg)) {
             TranslateMessage(&msg);
             DispatchMessage(&msg);
        }
        // --- 新增结束 ---
    }
    
    // 程序退出前最后一次清理
    if (!g_isExiting) {
         g_isExiting = TRUE; // 确保在 GetMessage 循环外退出时也标记
         StopSingBox(); 
    }
    
    if (g_isIconVisible) {
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }
    CleanupDynamicNodes();
    if (hMutex) CloseHandle(hMutex);
    UnregisterClassW(CLASS_NAME, hInstance);
    
    // --- 新增：清理字体 ---
    if (hLogFont) DeleteObject(hLogFont);
    // --- 新增结束 ---
    
    if (g_hFont) DeleteObject(g_hFont);
    return (int)msg.wParam;
}