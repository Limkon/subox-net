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
#include <wininet.h> 
#include <commctrl.h>
#include <time.h> 

#include "cJSON.c"

#ifndef NIF_GUID
#define NIF_GUID 0x00000020
#endif

#ifndef NOTIFYICON_VERSION_4
#define NOTIFYICON_VERSION_4 4
#endif

static const GUID APP_GUID = { 0xbfd8a583, 0x662a, 0x4fe3, { 0x97, 0x84, 0xfa, 0xb7, 0x8a, 0x33, 0x86, 0xa3 } };


#define WM_TRAY (WM_USER + 1)
#define WM_SINGBOX_CRASHED (WM_USER + 2)     
#define WM_LOG_UPDATE (WM_USER + 3)          
#define WM_INIT_COMPLETE (WM_USER + 5)       
#define WM_INIT_LOG (WM_USER + 6)            

#define ID_TRAY_EXIT 1001
#define ID_TRAY_AUTORUN 1002
#define ID_TRAY_SYSTEM_PROXY 1003
#define ID_TRAY_SETTINGS 1005
#define ID_TRAY_SHOW_CONSOLE 1007 
#define ID_TRAY_NODE_BASE 2000

#define ID_LOGVIEWER_EDIT 6001

#define ID_GLOBAL_HOTKEY 9001
#define ID_HOTKEY_CTRL 101

typedef struct {
    HWND hWndMain;
    BOOL isAutorun;
} INIT_THREAD_PARAMS;

NOTIFYICONDATAW nid;
HWND hwnd;
HMENU hMenu, hNodeSubMenu;
HANDLE hMutex = NULL;
PROCESS_INFORMATION pi = {0};
HFONT g_hFont = NULL; 

wchar_t** nodeTags = NULL;
int nodeCount = 0;
int nodeCapacity = 0;
wchar_t currentNode[64] = L"";
int httpPort = 0;

const wchar_t* REG_PATH_PROXY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";

BOOL g_isIconVisible = TRUE;
UINT g_hotkeyModifiers = 0;
UINT g_hotkeyVk = 0;
wchar_t g_iniFilePath[MAX_PATH] = {0};
wchar_t g_configUrl[2048] = {0}; 
wchar_t g_configFilePath[MAX_PATH] = {0};   
wchar_t g_tempConfigFilePath[MAX_PATH] = {0}; 

HANDLE hMonitorThread = NULL;           
HANDLE hLogMonitorThread = NULL;        
HANDLE hChildStd_OUT_Rd_Global = NULL;  
BOOL g_isExiting = FALSE;               

HWND hLogViewerWnd = NULL; 
HFONT hLogFont = NULL;     

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
char* ConvertLfToCrlf(const char* input);
BOOL DownloadConfig(const wchar_t* url, const wchar_t* savePath); 

void OpenLogViewerWindow();
LRESULT CALLBACK LogViewerWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

void ShowTrayTip(const wchar_t* title, const wchar_t* message) {
    if (!g_isIconVisible) return; 
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
    if (_wfopen_s(&f, filename, L"rb") != 0 || !f) { 
        *fileSize = 0;
        return FALSE; 
    }
    fseek(f, 0, SEEK_END);
    *fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (*fileSize <= 0) { 
        *fileSize = 0; 
        *buffer = NULL;
        fclose(f); 
        return FALSE; 
    }
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

void LoadSettings() {
    const wchar_t* defaultConfigUrl = L"";
    
    g_hotkeyModifiers = GetPrivateProfileIntW(L"Settings", L"Modifiers", 0, g_iniFilePath);
    g_hotkeyVk = GetPrivateProfileIntW(L"Settings", L"VK", 0, g_iniFilePath);
    g_isIconVisible = GetPrivateProfileIntW(L"Settings", L"ShowIcon", 1, g_iniFilePath);
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

BOOL ParseTags() {
    CleanupDynamicNodes();
    currentNode[0] = L'\0';
    httpPort = 0;
    char* buffer = NULL;
    long size = 0;
    if (!ReadFileToBuffer(g_configFilePath, &buffer, &size)) { 
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

DWORD WINAPI MonitorThread(LPVOID lpParam) {
    HANDLE hProcess = (HANDLE)lpParam;
    
    WaitForSingleObject(hProcess, INFINITE);

    if (!g_isExiting) {
        PostMessageW(hwnd, WM_SINGBOX_CRASHED, 0, 0);
    }

    return 0;
}

DWORD WINAPI LogMonitorThread(LPVOID lpParam) {
    char readBuf[4096];      
    char lineBuf[8192] = {0}; 
    DWORD dwRead;
    BOOL bSuccess;
    HANDLE hPipe = (HANDLE)lpParam;

    while (TRUE) {
        bSuccess = ReadFile(hPipe, readBuf, sizeof(readBuf) - 1, &dwRead, NULL);
        
        if (!bSuccess || dwRead == 0) {
            break; 
        }

        readBuf[dwRead] = '\0';

        if (hLogViewerWnd != NULL && !g_isExiting) {
            int wideLen = MultiByteToWideChar(CP_UTF8, 0, readBuf, -1, NULL, 0);
            if (wideLen > 0) {
                wchar_t* pWideBuf = (wchar_t*)malloc(wideLen * sizeof(wchar_t));
                if (pWideBuf) {
                    MultiByteToWideChar(CP_UTF8, 0, readBuf, -1, pWideBuf, wideLen);
                    
                    if (!PostMessageW(hLogViewerWnd, WM_LOG_UPDATE, 0, (LPARAM)pWideBuf)) {
                        free(pWideBuf);
                    }
                }
            }
        }

        strncat(lineBuf, readBuf, sizeof(lineBuf) - strlen(lineBuf) - 1);

        if (g_isExiting) {
            continue;
        }

        char* last_newline = strrchr(lineBuf, '\n');
        if (last_newline != NULL) {
            strcpy(lineBuf, last_newline + 1);
        } else if (strlen(lineBuf) > 4096) {
            lineBuf[0] = '\0';
        }
    }
    
    return 0;
}

void StartSingBox() {
    HANDLE hPipe_Rd_Local = NULL; 
    HANDLE hPipe_Wr_Local = NULL; 
    SECURITY_ATTRIBUTES sa;

    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&hPipe_Rd_Local, &hPipe_Wr_Local, &sa, 0)) {
        ShowError(L"管道创建失败", L"无法为核心程序创建输出管道。");
        return;
    }
    if (!SetHandleInformation(hPipe_Rd_Local, HANDLE_FLAG_INHERIT, 0)) {
        ShowError(L"管道句柄属性设置失败", L"无法设置输出管道读取句柄的属性。");
        CloseHandle(hPipe_Rd_Local);
        CloseHandle(hPipe_Wr_Local);
        return;
    }

    hChildStd_OUT_Rd_Global = hPipe_Rd_Local;

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hPipe_Wr_Local;
    si.hStdError = hPipe_Wr_Local;

    wchar_t cmdLine[MAX_PATH + 100]; 
    wsprintfW(cmdLine, L"sing-box.exe run -c \"%s\"", g_configFilePath); 

    if (!CreateProcessW(NULL, cmdLine, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        ShowError(L"核心程序启动失败", L"无法创建 sing-box.exe 进程。");
        ZeroMemory(&pi, sizeof(pi));
        CloseHandle(hChildStd_OUT_Rd_Global); 
        hChildStd_OUT_Rd_Global = NULL;
        CloseHandle(hPipe_Wr_Local);
        return;
    }

    CloseHandle(hPipe_Wr_Local);

    if (WaitForSingleObject(pi.hProcess, 500) == WAIT_OBJECT_0) {
        char chBuf[4096] = {0};
        DWORD dwRead = 0;
        wchar_t errorOutput[4096] = L"";

        if (ReadFile(hChildStd_OUT_Rd_Global, chBuf, sizeof(chBuf) - 1, &dwRead, NULL) && dwRead > 0) {
            chBuf[dwRead] = '\0';
            MultiByteToWideChar(CP_UTF8, 0, chBuf, -1, errorOutput, ARRAYSIZE(errorOutput));
        }

        wchar_t fullMessage[8192];
        wsprintfW(fullMessage, L"sing-box.exe 核心程序启动后立即退出。\n\n可能的原因:\n- 配置文件(config.dat)格式错误\n- 核心文件损坏或不兼容\n\n核心程序输出:\n%s", errorOutput); 
        ShowError(L"核心程序启动失败", fullMessage);
        
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        ZeroMemory(&pi, sizeof(pi));
        
        CloseHandle(hChildStd_OUT_Rd_Global); 
        hChildStd_OUT_Rd_Global = NULL;
    } 
    else {
        hMonitorThread = CreateThread(NULL, 0, MonitorThread, pi.hProcess, 0, NULL);
        
        HANDLE hPipeForLogThread;
        if (DuplicateHandle(GetCurrentProcess(), hChildStd_OUT_Rd_Global,
                           GetCurrentProcess(), &hPipeForLogThread, 0,
                           FALSE, DUPLICATE_SAME_ACCESS))
        {
            hLogMonitorThread = CreateThread(NULL, 0, LogMonitorThread, hPipeForLogThread, 0, NULL);
        }
    }
}
void SwitchNode(const wchar_t* tag) {
    SafeReplaceOutbound(tag);
    wcsncpy(currentNode, tag, ARRAYSIZE(currentNode) - 1);
    currentNode[ARRAYSIZE(currentNode)-1] = L'\0';
    
    g_isExiting = TRUE; 
    StopSingBox();
    g_isExiting = FALSE; 

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

void SafeReplaceOutbound(const wchar_t* newTag) {
    char* buffer = NULL;
    long size = 0;
    if (!ReadFileToBuffer(g_configFilePath, &buffer, &size)) { 
        MessageBoxW(NULL, L"无法打开 config.dat", L"错误", MB_OK | MB_ICONERROR); 
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

    cJSON* route = cJSON_GetObjectItem(root, "route");
    if (route) {
        cJSON* final_outbound = cJSON_GetObjectItem(route, "final");
        if (final_outbound) {
            cJSON_SetValuestring(final_outbound, newTagMb);
        } else {
            cJSON_AddItemToObject(route, "final", cJSON_CreateString(newTagMb));
        }
    }

    char* newContent = cJSON_PrintBuffered(root, 1, 1);

    if (newContent) {
        FILE* out = NULL;
        if (_wfopen_s(&out, g_configFilePath, L"wb") == 0 && out != NULL) { 
            fwrite(newContent, 1, strlen(newContent), out);
            fclose(out);
        }
        free(newContent);
    }
    cJSON_Delete(root);
    free(buffer);
    free(newTagMb);
}
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
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_AUTORUN, L"开机启动");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SYSTEM_PROXY, L"系统代理");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SETTINGS, L"隐藏图标");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SHOW_CONSOLE, L"显示日志"); 
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"退出");
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static time_t lastAutoRestart = 0;
    const time_t RESTART_COOLDOWN = 60; 

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
            
            g_isExiting = TRUE; 

            if (hLogViewerWnd != NULL) {
                DestroyWindow(hLogViewerWnd);
            }

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
        else if (id == ID_TRAY_SETTINGS) {
            OpenSettingsWindow();
        } 
        else if (id == ID_TRAY_SHOW_CONSOLE) { 
            OpenLogViewerWindow();
        } else if (id >= ID_TRAY_NODE_BASE && id < ID_TRAY_NODE_BASE + nodeCount) {
            SwitchNode(nodeTags[id - ID_TRAY_NODE_BASE]);
        }
    } else if (msg == WM_HOTKEY) {
        if (wParam == ID_GLOBAL_HOTKEY) {
            ToggleTrayIconVisibility();
        }
    }
    else if (msg == WM_SINGBOX_CRASHED) {
        ShowTrayTip(L"Sing-box 监控", L"核心进程意外终止。请手动检查。");
    }
    
    else if (msg == WM_INIT_LOG) {
        if (hLogViewerWnd != NULL) {
            if (!PostMessageW(hLogViewerWnd, WM_LOG_UPDATE, 0, lParam)) {
                free((void*)lParam);
            }
        } else {
            free((void*)lParam);
        }
    }
    
    else if (msg == WM_INIT_COMPLETE) {
        BOOL success = (BOOL)wParam; 
        if (success) {
            ParseTags();
            
            wcsncpy(nid.szTip, L"程序正在运行...", ARRAYSIZE(nid.szTip) - 1);
            if(g_isIconVisible) { Shell_NotifyIconW(NIM_MODIFY, &nid); }
            
            ShowTrayTip(L"启动成功", L"程序已准备就绪。");

        } else {
            ShowTrayTip(L"启动失败", L"核心初始化失败，程序将退出。");
            
            PostMessageW(hWnd, WM_COMMAND, ID_TRAY_EXIT, 0);
        }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
void StopSingBox() {
    g_isExiting = TRUE; 

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
    
    if (hMonitorThread) {
        WaitForSingleObject(hMonitorThread, 1000);
        CloseHandle(hMonitorThread);
    }

    if (hChildStd_OUT_Rd_Global) {
        CloseHandle(hChildStd_OUT_Rd_Global);
    }
    if (hLogMonitorThread) {
        WaitForSingleObject(hLogMonitorThread, 1000);
        CloseHandle(hLogMonitorThread);
    }

    ZeroMemory(&pi, sizeof(pi));
    hMonitorThread = NULL;
    hLogMonitorThread = NULL;
    hChildStd_OUT_Rd_Global = NULL;
    
}

void SetAutorun(BOOL enable) {
    HKEY hKey;
    wchar_t path[MAX_PATH];
    wchar_t pathWithArg[MAX_PATH + 20]; 

    GetModuleFileNameW(NULL, path, MAX_PATH);
    RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
    if (hKey) {
        if (enable) {
            wsprintfW(pathWithArg, L"\"%s\" /autorun", path);
            RegSetValueExW(hKey, L"singbox_tray", 0, REG_SZ, (BYTE*)pathWithArg, (wcslen(pathWithArg) + 1) * sizeof(wchar_t));
        } else {
            RegDeleteValueW(hKey, L"singbox_tray");
        }
        RegCloseKey(hKey);
    }
}

BOOL IsAutorunEnabled() {
    HKEY hKey;
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH); 

    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t value[MAX_PATH + 50]; 
        DWORD size = sizeof(value);
        LONG res = RegQueryValueExW(hKey, L"singbox_tray", NULL, NULL, (LPBYTE)value, &size);
        RegCloseKey(hKey);

        if (res == ERROR_SUCCESS) {
            if (wcsstr(value, exePath) != NULL) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

BOOL DownloadConfig(const wchar_t* url, const wchar_t* savePath) {
    wchar_t cmdLine[4096]; 
    wchar_t fullSavePath[MAX_PATH];
    wchar_t fullCurlPath[MAX_PATH];
    wchar_t moduleDir[MAX_PATH];

    GetModuleFileNameW(NULL, moduleDir, MAX_PATH);
    wchar_t* p = wcsrchr(moduleDir, L'\\');
    if (p) {
        *p = L'\0'; 
    } else {
        wcsncpy(moduleDir, L".", MAX_PATH);
    }

    wsprintfW(fullCurlPath, L"%s\\curl.exe", moduleDir);

    DWORD fileAttr = GetFileAttributesW(fullCurlPath);
    if (fileAttr == INVALID_FILE_ATTRIBUTES || (fileAttr & FILE_ATTRIBUTE_DIRECTORY)) {
         wchar_t errorMsg[MAX_PATH + 256];
         wsprintfW(errorMsg, L"启动失败：未找到 curl.exe。\n\n"
                            L"请确保 curl.exe 位于此路径：\n%s",
                            fullCurlPath);
         MessageBoxW(NULL, errorMsg, L"文件缺失", MB_OK | MB_ICONERROR);
        return FALSE;
    }

    if (GetFullPathNameW(savePath, MAX_PATH, fullSavePath, NULL) == 0) {
        ShowError(L"下载失败", L"无法验证配置文件的绝对路径。");
        return FALSE;
    }

    wsprintfW(cmdLine, 
        L"\"%s\" -ksSL -o \"%s\" \"%s\"", 
        fullCurlPath, fullSavePath, url
    );

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION downloaderPi = {0};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE; 

    if (!CreateProcessW(NULL,           
                        cmdLine,        
                        NULL,           
                        NULL,           
                        FALSE,          
                        CREATE_NO_WINDOW, 
                        NULL,           
                        moduleDir,      
                        &si,            
                        &downloaderPi)) 
    {
        ShowError(L"下载失败", L"无法启动 curl.exe 下载进程 (CreateProcessW)。");
        return FALSE;
    }

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
        wchar_t errorMsg[512];
        wsprintfW(errorMsg, L"curl.exe 报告了错误 (退出码 %lu)。\n请检查网络或 URL 是否正确。", exitCode);
        ShowError(L"下载失败", errorMsg);
        return FALSE;
    }

    long fileSize = 0;
    char* fileBuffer = NULL;
    if (ReadFileToBuffer(savePath, &fileBuffer, &fileSize)) {
        if (fileSize < 50) { 
             ShowError(L"下载失败", L"下载的文件过小 (小于 50 字节)。\n"
                                   L"这可能是一个错误页面，请检查 URL 是否为[原始]链接。");
             free(fileBuffer);
             return FALSE;
        }
        free(fileBuffer);
        return TRUE; 
    } else {
        ShowError(L"下载失败", L"curl.exe 报告成功，但无法读取下载的临时配置文件。"); 
        return FALSE;
    }
}

LRESULT CALLBACK LogViewerWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hEdit = NULL;
    const int MAX_LOG_LENGTH = 200000;  
    const int TRIM_LOG_LENGTH = 100000; 

    switch (msg) {
        case WM_CREATE: {
            hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                    ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                                    0, 0, 0, 0, 
                                    hWnd, (HMENU)ID_LOGVIEWER_EDIT,
                                    GetModuleHandle(NULL), NULL);
            
            if (hEdit == NULL) {
                ShowError(L"创建失败", L"无法创建日志显示框。");
                return -1; 
            }
            
            SendMessage(hEdit, WM_SETFONT, (WPARAM)hLogFont, TRUE);
            break;
        }

        case WM_LOG_UPDATE: {
            wchar_t* pLogChunk = (wchar_t*)lParam;
            if (pLogChunk) {
                int textLen = GetWindowTextLengthW(hEdit);
                if (textLen > MAX_LOG_LENGTH) {
                    SendMessageW(hEdit, EM_SETSEL, 0, TRIM_LOG_LENGTH);
                    SendMessageW(hEdit, EM_REPLACESEL, 0, (LPARAM)L"[... 日志已裁剪 ...]\r\n");
                }

                SendMessageW(hEdit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1); 
                SendMessageW(hEdit, EM_REPLACESEL, 0, (LPARAM)pLogChunk); 
                
                free(pLogChunk);
            }
            break;
        }

        case WM_SIZE: {
            RECT rcClient;
            GetClientRect(hWnd, &rcClient);
            MoveWindow(hEdit, 0, 0, rcClient.right, rcClient.bottom, TRUE);
            break;
        }

        case WM_CLOSE: {
            ShowWindow(hWnd, SW_HIDE);
            hLogViewerWnd = NULL; 
            break;
        }

        case WM_DESTROY: {
            hLogViewerWnd = NULL;
            break;
        }

        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

void OpenLogViewerWindow() {
    if (hLogViewerWnd != NULL) {
        ShowWindow(hLogViewerWnd, SW_SHOW);
        SetForegroundWindow(hLogViewerWnd);
        return;
    }

    const wchar_t* LOGVIEWER_CLASS_NAME = L"SingboxLogViewerClass";
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = LogViewerWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = LOGVIEWER_CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(GetModuleHandle(NULL), MAKEINTRESOURCE(1)); 
    if (wc.hIcon == NULL) {
        wc.hIcon = LoadIconW(NULL, IDI_APPLICATION); 
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
        hwnd, 
        NULL, wc.hInstance, NULL
    );

    if (hLogViewerWnd) {
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

DWORD WINAPI InitThread(LPVOID lpParam) {
    INIT_THREAD_PARAMS* pParams = (INIT_THREAD_PARAMS*)lpParam;
    HWND hWndMain = pParams->hWndMain;
    BOOL isAutorun = pParams->isAutorun;
    
    free(pParams); 
    pParams = NULL; 
    
    #define THREAD_CLEANUP_AND_EXIT(success) \
        do { \
            PostMessageW(hWndMain, WM_INIT_COMPLETE, (WPARAM)(success), (LPARAM)0); \
            return (success) ? 0 : 1; \
        } while (0)

    wchar_t tempPathBuffer[MAX_PATH];
    DWORD tempPathLen = GetTempPathW(MAX_PATH, tempPathBuffer);
    if (tempPathLen == 0 || tempPathLen > MAX_PATH) {
        ShowError(L"启动失败", L"无法获取系统临时目录 (TEMP) 路径。");
        THREAD_CLEANUP_AND_EXIT(FALSE);
    }
    wsprintfW(g_configFilePath, L"%sconfig.dat", tempPathBuffer);
    wsprintfW(g_tempConfigFilePath, L"%sconfig.dat.tmp", tempPathBuffer);
    
    int retryCount = 0;
    const int maxRetries = 5; 
    const int retryDelay = 15000; 

    BOOL downloadSuccess = DownloadConfig(g_configUrl, g_tempConfigFilePath);

    if (!downloadSuccess && isAutorun) {
        PostMessageW(hWndMain, WM_INIT_LOG, 0, (LPARAM)_wcsdup(L"[InitThread] 首次下载失败 (开机启动)，15秒后重试...\r\n"));

        while (retryCount < maxRetries) {
            Sleep(retryDelay); 
            retryCount++;
            
            wchar_t retryMsg[100];
            wsprintfW(retryMsg, L"[InitThread] 正在进行第 %d/%d 次重试...\r\n", retryCount, maxRetries);
            PostMessageW(hWndMain, WM_INIT_LOG, 0, (LPARAM)_wcsdup(retryMsg));

            downloadSuccess = DownloadConfig(g_configUrl, g_tempConfigFilePath);
            if (downloadSuccess) {
                PostMessageW(hWndMain, WM_INIT_LOG, 0, (LPARAM)_wcsdup(L"[InitThread] 重试下载成功。\r\n"));
                break; 
            } else {
                wchar_t failMsg[100];
                wsprintfW(failMsg, L"[InitThread] 第 %d/%d 次重试失败。\r\n", retryCount, maxRetries);
                PostMessageW(hWndMain, WM_INIT_LOG, 0, (LPARAM)_wcsdup(failMsg));
            }
        }
    }
    
    if (!downloadSuccess) {
        THREAD_CLEANUP_AND_EXIT(FALSE);
    }

    DWORD fileAttr = GetFileAttributesW(g_configFilePath);
    BOOL configExists = (fileAttr != INVALID_FILE_ATTRIBUTES && !(fileAttr & FILE_ATTRIBUTE_DIRECTORY));

    if (configExists) {
        if (!ParseTags()) { 
            wchar_t errorMsg[MAX_PATH + 256];
            wsprintfW(errorMsg, L"无法读取或解析本地 config.dat 文件。\n\n请删除 %s 后重试。", g_configFilePath);
            MessageBoxW(NULL, errorMsg, L"本地配置解析失败", MB_OK | MB_ICONERROR); 
            DeleteFileW(g_tempConfigFilePath); 
            THREAD_CLEANUP_AND_EXIT(FALSE);
        }
        
        g_isExiting = FALSE;
        StartSingBox();

        long oldSize = 0;
        char* oldBuf = NULL;
        ReadFileToBuffer(g_configFilePath, &oldBuf, &oldSize); 
        if (oldBuf) free(oldBuf);
            
        long newSize = 0;
        char* newBuf = NULL;
        ReadFileToBuffer(g_tempConfigFilePath, &newBuf, &newSize); 
        if (newBuf) free(newBuf);

        if (newSize > 0 && abs(newSize - oldSize) > 100) {
            if (MoveFileExW(g_tempConfigFilePath, g_configFilePath, MOVEFILE_REPLACE_EXISTING)) {
            } else {
                DeleteFileW(g_tempConfigFilePath);
            }
        } else {
            DeleteFileW(g_tempConfigFilePath);
        }

    } else {
        if (!MoveFileExW(g_tempConfigFilePath, g_configFilePath, MOVEFILE_REPLACE_EXISTING)) {
             ShowError(L"启动失败", L"无法将下载的配置 (tmp) 重命名为 config.dat。");
             DeleteFileW(g_tempConfigFilePath);
             THREAD_CLEANUP_AND_EXIT(FALSE);
        }
        
        if (!ParseTags()) { 
            MessageBoxW(NULL, L"无法读取或解析下载的 config.dat 文件。\n\n该文件是否为有效的JSON格式？", L"配置解析失败", MB_OK | MB_ICONERROR);
            THREAD_CLEANUP_AND_EXIT(FALSE);
        }
        
        g_isExiting = FALSE;
        StartSingBox();
    }
    
    #undef THREAD_CLEANUP_AND_EXIT
    
    PostMessageW(hWndMain, WM_INIT_COMPLETE, (WPARAM)TRUE, (LPARAM)0);
    return 0;
}


int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR lpCmdLine, int nCmdShow) {
    wchar_t mutexName[128];
    wchar_t guidString[40];

    g_hFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"宋体");

    hLogFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (hLogFont == NULL) {
        hLogFont = (HFONT)GetStockObject(SYSTEM_FIXED_FONT); 
    }

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
        if (hLogFont) DeleteObject(hLogFont); 
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
        SetCurrentDirectoryW(szPath); 
        wcsncpy(g_iniFilePath, szPath, MAX_PATH - 1);
        g_iniFilePath[MAX_PATH - 1] = L'\0';
        wcsncat(g_iniFilePath, L"\\set.ini", MAX_PATH - wcslen(g_iniFilePath) - 1);
    } else {
        wcsncpy(g_iniFilePath, L"set.ini", MAX_PATH - 1);
    }
    
    #define CLEANUP_AND_EXIT() \
        do { \
            if (hMutex) CloseHandle(hMutex); \
            if (g_hFont) DeleteObject(g_hFont); \
            if (hLogFont) DeleteObject(hLogFont); \
            if (hwnd) DestroyWindow(hwnd); \
            PostQuitMessage(1); \
            return 1; \
        } while (0)

    
    LoadSettings();

    const wchar_t* CLASS_NAME = L"TrayWindowClass";
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCE(1));
    if (!wc.hIcon) {
        wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    }
    RegisterClassW(&wc);
    hwnd = CreateWindowExW(0, CLASS_NAME, L"TrayApp", 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (!hwnd) {
        if (g_hFont) DeleteObject(g_hFont);
        if (hLogFont) DeleteObject(hLogFont); 
        return 1;
    }

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

    if (g_isIconVisible) {
        Shell_NotifyIconW(NIM_ADD, &nid);
    }

    
    INIT_THREAD_PARAMS* pInitParams = (INIT_THREAD_PARAMS*)malloc(sizeof(INIT_THREAD_PARAMS));
    if (pInitParams == NULL) {
        ShowError(L"致命错误", L"无法分配启动参数内存。");
        if (g_isIconVisible) Shell_NotifyIconW(NIM_DELETE, &nid);
        CLEANUP_AND_EXIT();
    }
    
    pInitParams->hWndMain = hwnd;
    pInitParams->isAutorun = (wcsstr(lpCmdLine, L"/autorun") != NULL);

    HANDLE hInitThread = CreateThread(NULL, 0, InitThread, (LPVOID)pInitParams, 0, NULL);
    if (hInitThread) {
        CloseHandle(hInitThread); 
    } else {
        ShowError(L"致命错误", L"无法创建启动线程。");
        if (g_isIconVisible) Shell_NotifyIconW(NIM_DELETE, &nid);
        free(pInitParams); 
        CLEANUP_AND_EXIT();
    }
    
    #undef CLEANUP_AND_EXIT

    
    wcsncpy(nid.szTip, L"程序正在启动... (下载配置)", ARRAYSIZE(nid.szTip) - 1);
    if(g_isIconVisible) { Shell_NotifyIconW(NIM_MODIFY, &nid); }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (hLogViewerWnd == NULL || !IsDialogMessageW(hLogViewerWnd, &msg)) {
             TranslateMessage(&msg);
             DispatchMessage(&msg);
        }
    }
    
    if (!g_isExiting) {
         g_isExiting = TRUE; 
         StopSingBox(); 
    }
    
    if (g_isIconVisible) {
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }
    CleanupDynamicNodes();
    if (hMutex) CloseHandle(hMutex);
    UnregisterClassW(CLASS_NAME, hInstance);
    
    if (hLogFont) DeleteObject(hLogFont);
    
    if (g_hFont) DeleteObject(g_hFont);
    return (int)msg.wParam;
}