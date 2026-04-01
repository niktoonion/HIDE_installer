/*
 * Copyright (C) 2026 Fedotov Vladislav Igorevich (niktoonion)
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see https://www.gnu.org/licenses/.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/********************************************************************
*  HIDE Installer – один‑файловый Win32‑установщик
*  -------------------------------------------------
*  Что делает:
*  • Скачивает HIDE.jar, icon.ico, icon.jpg и version.txt
*  • Устанавливает программу в %ProgramFiles%\HIDE
*  • Ярлыки (рабочий стол, меню Пуск) используют icon.ico
*  • Прикрепляет ярлык к панели задач (taskbar‑pin)
*  • Принудительно «pin‑ит» ярлык в раздел Pinned меню Пуск (start‑pin)
*  • Регистрация в «Программы и компоненты» (DisplayIcon = icon.ico)
*  • Встроенный деинсталлятор (параметр /uninstall + отдельный ярлык)
*  • Окно установщика имеет собственную иконку‑ресурс
*
*  Сборка (пример):
*      cl /EHsc /DUNICODE /D_UNICODE /O2 hide_installer.cpp ^
*          user32.lib advapi32.lib shell32.lib ole32.lib urlmon.lib ^
*          comctl32.lib shlwapi.lib installer.res
*
********************************************************************/

#define UNICODE
#define _UNICODE

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <urlmon.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <string>
#include <fstream>
#include <thread>
#include "resource.h"                     // IDI_APPICON

#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")

// ------------------------------------------------------------------
// Константы (можно менять)

const std::wstring JAR_URL = L"https://github.com/niktoonion/HentaiIDE/releases/download/alpha/HIDE.jar";
const std::wstring ICON_ICO_URL = L"https://github.com/niktoonion/HentaiIDE/releases/download/alpha/icon.ico";
const std::wstring ICON_JPG_URL = L"https://github.com/niktoonion/HentaiIDE/releases/download/alpha/icon.jpg"; // ← скачиваем, но не используем
const std::wstring VERSION_TXT_URL = L"https://github.com/niktoonion/HentaiIDE/releases/download/alpha/version.txt";

const std::wstring JAVA_INSTALLER_URL = L"https://javadl.oracle.com/webapps/download/AutoDL?BundleId=252907_0d06828d282343ea81775b28020a7cd3";

const std::wstring CURRENT_VERSION = L"0.0.1";
const std::wstring APP_NAME = L"HIDE";
const std::wstring DISPLAY_NAME = L"HIDE Code Editor";
const std::wstring COMPANY_NAME = L"niktoonion";

// UI‑идентификаторы
enum {
    ID_BTN_INSTALL = 1001,
    ID_BTN_UPDATE = 1002,
    ID_BTN_UNINSTALL = 1003,
    ID_BTN_EXIT = 1004,
    ID_PROGRESS = 2001
};

// ------------------------------------------------------------------
// Утилиты -----------------------------------------------------------

static std::wstring GetErrorMessage(DWORD err)
{
    LPWSTR buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPWSTR)&buf, 0, nullptr);
    std::wstring msg = buf ? buf : L"";
    LocalFree(buf);
    return msg;
}

// Скачивание через URLDownloadToFileW
bool DownloadFile(const std::wstring& url,
    const std::wstring& destPath)
{
    // Если уже есть – не скачиваем повторно
    if (PathFileExistsW(destPath.c_str())) return true;
    HRESULT hr = URLDownloadToFileW(nullptr,
        url.c_str(),
        destPath.c_str(),
        0,
        nullptr);
    return SUCCEEDED(hr);
}

// Создать каталог, если его нет
bool EnsureDirectory(const std::wstring& dir)
{
    if (PathFileExistsW(dir.c_str())) return true;
    return CreateDirectoryW(dir.c_str(), nullptr) != 0;
}

// Путь к известной папке (CSIDL)
std::wstring GetKnownFolderPath(int csidl)
{
    WCHAR path[MAX_PATH];
    if (SHGetFolderPathW(nullptr, csidl, nullptr,
        SHGFP_TYPE_CURRENT, path) == S_OK)
        return std::wstring(path);
    return L"";
}

// Проверка прав администратора (нужны для записи в Program Files)
bool IsRunningAsAdmin()
{
    BOOL admin = FALSE;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(),
        TOKEN_QUERY,
        &token))
    {
        TOKEN_ELEVATION e{};
        DWORD sz = 0;
        if (GetTokenInformation(token,
            TokenElevation,
            &e,
            sizeof(e),
            &sz))
            admin = e.TokenIsElevated;
        CloseHandle(token);
    }
    return (admin != FALSE);
}

// ------------------------------------------------------------------
// Java‑related

bool IsJavaInstalled()
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(L"C:\\Windows\\System32\\cmd.exe",
        (LPWSTR)L"/C java -version",
        nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr,
        nullptr, &si, &pi);
    if (!ok) return false;

    WaitForSingleObject(pi.hProcess, 5000);
    DWORD ec = 0;
    GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (ec == 0);
}

// Запуск установщика JRE‑8 (если URL задан)
bool InstallJava()
{
    if (JAVA_INSTALLER_URL.empty()) return false;

    WCHAR tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring installer = std::wstring(tmp) + L"jre8_installer.exe";

    if (!DownloadFile(JAVA_INSTALLER_URL, installer))
        return false;

    HINSTANCE rc = ShellExecuteW(nullptr,
        L"runas",
        installer.c_str(),
        nullptr,
        nullptr,
        SW_SHOWNORMAL);
    return (reinterpret_cast<int>(rc) > 32);
}

// ------------------------------------------------------------------
// Ярлыки

bool CreateShortcut(const std::wstring& target,
    const std::wstring& args,
    const std::wstring& shortcutPath,
    const std::wstring& description = L"",
    const std::wstring& iconPath = L"",
    int iconIdx = 0)
{
    CoInitialize(nullptr);
    IShellLinkW* psl = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&psl));
    if (SUCCEEDED(hr))
    {
        psl->SetPath(target.c_str());
        psl->SetArguments(args.c_str());
        if (!description.empty())
            psl->SetDescription(description.c_str());

        // Указываем путь к icon.ico (для всех ярлыков)
        if (!iconPath.empty())
            psl->SetIconLocation(iconPath.c_str(), iconIdx);

        IPersistFile* ppf = nullptr;
        hr = psl->QueryInterface(IID_PPV_ARGS(&ppf));
        if (SUCCEEDED(hr))
        {
            hr = ppf->Save(shortcutPath.c_str(), TRUE);
            ppf->Release();
        }
        psl->Release();
    }
    CoUninitialize();
    return SUCCEEDED(hr);
}

// Прикрепление ярлыка к панели задач (verb "taskbarpin")
bool PinToTaskbar(const std::wstring& shortcutPath)
{
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_INVOKEIDLIST | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"taskbarpin";
    sei.lpFile = shortcutPath.c_str();
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei);
}

// Принудительное «pin‑ить» в раздел «Pinned» меню Пуск (verb "startpin")
bool PinToStartMenu(const std::wstring& shortcutPath)
{
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_INVOKEIDLIST | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"startpin";
    sei.lpFile = shortcutPath.c_str();
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei);
}

// Снять «pin» из меню Пуск (verb "startunpin") – используется при удалении
bool UnpinFromStartMenu(const std::wstring& shortcutPath)
{
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_INVOKEIDLIST | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"startunpin";
    sei.lpFile = shortcutPath.c_str();
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei);
}

// ------------------------------------------------------------------
// Регистрация в «Программы и компоненты»

bool RegisterUninstallInfo(const std::wstring& installDir,
    const std::wstring& exePath,
    const std::wstring& iconPath)
{
    HKEY hKey;
    std::wstring subKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\HIDE";

    LONG rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
        subKey.c_str(),
        0, nullptr,
        REG_OPTION_VOLATILE,
        KEY_WRITE,
        nullptr,
        &hKey,
        nullptr);
    if (rc != ERROR_SUCCESS) return false;

    std::wstring disp = DISPLAY_NAME;
    RegSetValueExW(hKey, L"DisplayName", 0, REG_SZ,
        (const BYTE*)disp.c_str(),
        (DWORD)((disp.size() + 1) * sizeof(wchar_t)));

    std::wstring uninstallCmd = L"\"" + exePath + L"\" /uninstall";
    RegSetValueExW(hKey, L"UninstallString", 0, REG_SZ,
        (const BYTE*)uninstallCmd.c_str(),
        (DWORD)((uninstallCmd.size() + 1) * sizeof(wchar_t)));

    // DisplayIcon = путь к icon.ico
    RegSetValueExW(hKey, L"DisplayIcon", 0, REG_SZ,
        (const BYTE*)iconPath.c_str(),
        (DWORD)((iconPath.size() + 1) * sizeof(wchar_t)));

    RegCloseKey(hKey);
    return true;
}

// ------------------------------------------------------------------
// Проверка обновлений

bool IsUpdateAvailable(std::wstring& remoteVersion)
{
    WCHAR tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring localCopy = std::wstring(tmp) + L"hide_version.txt";

    if (!DownloadFile(VERSION_TXT_URL, localCopy))
        return false;

    std::wifstream fin(localCopy);
    if (!fin) return false;
    std::getline(fin, remoteVersion);
    fin.close();

    remoteVersion.erase(remoteVersion.find_last_not_of(L" \r\n\t") + 1);
    return (remoteVersion != CURRENT_VERSION);
}

// ------------------------------------------------------------------
// Деинсталляция

bool PerformUninstall()
{
    std::wstring progFiles = GetKnownFolderPath(CSIDL_PROGRAM_FILES);
    std::wstring installDir = progFiles + L"\\HIDE";

    std::wstring desktop = GetKnownFolderPath(CSIDL_DESKTOPDIRECTORY);
    std::wstring startMenu = GetKnownFolderPath(CSIDL_COMMON_PROGRAMS) + L"\\HIDE";

    // Убираем «Pinned»‑раздел (если пользователь уже навесил)
    UnpinFromStartMenu(startMenu + L"\\HIDE.lnk");
    UnpinFromStartMenu(startMenu + L"\\Uninstall HIDE.lnk");

    // Удаляем ярлыки
    DeleteFileW((desktop + L"\\HIDE.lnk").c_str());
    DeleteFileW((desktop + L"\\Uninstall HIDE.lnk").c_str());
    DeleteFileW((startMenu + L"\\HIDE.lnk").c_str());
    DeleteFileW((startMenu + L"\\Uninstall HIDE.lnk").c_str());

    // Удаляем файлы программы
    DeleteFileW((installDir + L"\\HIDE.jar").c_str());
    DeleteFileW((installDir + L"\\icon.ico").c_str());
    DeleteFileW((installDir + L"\\icon.jpg").c_str());   // jpg‑файл, который мы скачали, просто стираем

    // Папка (если пустая)
    RemoveDirectoryW(installDir.c_str());

    // Убираем запись из реестра
    RegDeleteKeyW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\HIDE");

    return true;
}

// ------------------------------------------------------------------
// Основная операция установки

bool DoInstall(HWND hProgress)
{
    // 1. Требуются права администратора
    if (!IsRunningAsAdmin())
    {
        MessageBoxW(nullptr,
            L"Для установки требуются права администратора.",
            L"Error", MB_ICONERROR);
        return false;
    }

    // 2. Папка %ProgramFiles%\HIDE
    std::wstring progFiles = GetKnownFolderPath(CSIDL_PROGRAM_FILES);
    std::wstring installDir = progFiles + L"\\HIDE";
    EnsureDirectory(installDir);

    // 3. Скачиваем JAR, icon.ico и **дополнительный** icon.jpg
    std::wstring jarPath = installDir + L"\\HIDE.jar";
    std::wstring icoPath = installDir + L"\\icon.ico";
    std::wstring jpgPath = installDir + L"\\icon.jpg";   // ← будет скачан, но не будет использоваться

    // Последовательные (но «параллельные» по смыслу) загрузки
    SendMessageW(hProgress, PBM_SETPOS, 0, 0);
    if (!DownloadFile(JAR_URL, jarPath))
    {
        MessageBoxW(nullptr, L"Не удалось скачать HIDE.jar", L"Error", MB_ICONERROR);
        return false;
    }
    SendMessageW(hProgress, PBM_SETPOS, 20, 0);

    if (!DownloadFile(ICON_ICO_URL, icoPath))
    {
        MessageBoxW(nullptr, L"Не удалось скачать icon.ico", L"Error", MB_ICONERROR);
        return false;
    }
    SendMessageW(hProgress, PBM_SETPOS, 40, 0);

    // Скачиваем jpg, но НЕ используем её нигде
    if (!DownloadFile(ICON_JPG_URL, jpgPath))
    {
        MessageBoxW(nullptr, L"Не удалось скачать icon.jpg (всё равно установимся).",
            L"Warning", MB_ICONWARNING);
        // Ошибку можно игнорировать – она не критична
    }
    SendMessageW(hProgress, PBM_SETPOS, 60, 0);

    // 4. Проверяем наличие Java
    if (!IsJavaInstalled())
    {
        int rc = MessageBoxW(nullptr,
            L"Java 8 не найдена. Установить сейчас?",
            L"Java not found", MB_YESNO | MB_ICONQUESTION);
        if (rc == IDYES)
        {
            if (!InstallJava())
            {
                MessageBoxW(nullptr,
                    L"Не удалось запустить установщик Java.", L"Error", MB_ICONERROR);
                return false;
            }
            MessageBoxW(nullptr,
                L"После установки Java запустите установщик ещё раз.", L"Info", MB_ICONINFORMATION);
            return true;    // пользователь должен перезапустить установщик
        }
    }

    // 5. Ярлыки (везде указываем путь к icon.ico)
    std::wstring javaExe = L"javaw.exe";
    std::wstring args = L"-jar \"" + jarPath + L"\"";

    // 5.1 Ярлык на рабочем столе
    std::wstring desktop = GetKnownFolderPath(CSIDL_DESKTOPDIRECTORY);
    std::wstring desktopLnk = desktop + L"\\HIDE.lnk";
    CreateShortcut(javaExe, args, desktopLnk,
        DISPLAY_NAME, icoPath);

    // 5.2 Ярлык в меню Пуск (общая папка)
    std::wstring startMenu = GetKnownFolderPath(CSIDL_COMMON_PROGRAMS) + L"\\HIDE";
    EnsureDirectory(startMenu);
    std::wstring startLnk = startMenu + L"\\HIDE.lnk";
    CreateShortcut(javaExe, args, startLnk,
        DISPLAY_NAME, icoPath);

    // 5.3 Ярлык деинсталлятора (тоже с icon.ico)
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring uninstallLnk = startMenu + L"\\Uninstall HIDE.lnk";
    CreateShortcut(exePath, L"/uninstall", uninstallLnk,
        L"Uninstall " + DISPLAY_NAME, icoPath);

    // 5.4 Прикрепляем основной ярлык к панели задач
    PinToTaskbar(startLnk);

    // 5.5 Принудительно «pin‑им» в раздел «Pinned» меню Пуск
    // (это делается через verb “startpin”, который не требует пользовательского подтверждения,
    // но в редких случаях может быть отклонён системой.)
    PinToStartMenu(startLnk);

    // 6. Регистрация в «Программы и компоненты» (icon.ico)
    RegisterUninstallInfo(installDir, exePath, icoPath);

    SendMessageW(hProgress, PBM_SETPOS, 100, 0);
    return true;
}

// ------------------------------------------------------------------
// UI ---------------------------------------------------------------

static HWND g_hProgress = nullptr;
static HWND g_hBtnInstall = nullptr;
static HWND g_hBtnUpdate = nullptr;
static HWND g_hBtnUninstall = nullptr;
static HWND g_hBtnExit = nullptr;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg,
    WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        CreateWindowW(L"STATIC", L"HIDE Installer",
            WS_CHILD | WS_VISIBLE,
            20, 20, 260, 20, hwnd, nullptr, nullptr, nullptr);

        g_hBtnInstall = CreateWindowW(L"BUTTON", L"Установить",
            WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
            20, 60, 90, 30, hwnd, (HMENU)ID_BTN_INSTALL,
            nullptr, nullptr);

        g_hBtnUpdate = CreateWindowW(L"BUTTON", L"Обновить",
            WS_TABSTOP | WS_VISIBLE | WS_CHILD,
            120, 60, 90, 30, hwnd, (HMENU)ID_BTN_UPDATE,
            nullptr, nullptr);

        g_hBtnUninstall = CreateWindowW(L"BUTTON", L"Удалить",
            WS_TABSTOP | WS_VISIBLE | WS_CHILD,
            220, 60, 90, 30, hwnd, (HMENU)ID_BTN_UNINSTALL,
            nullptr, nullptr);

        g_hBtnExit = CreateWindowW(L"BUTTON", L"Выход",
            WS_TABSTOP | WS_VISIBLE | WS_CHILD,
            320, 60, 60, 30, hwnd, (HMENU)ID_BTN_EXIT,
            nullptr, nullptr);

        // Прогресс‑бар
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS };
        InitCommonControlsEx(&icc);
        g_hProgress = CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
            WS_CHILD | WS_VISIBLE,
            20, 110, 380, 20, hwnd, (HMENU)ID_PROGRESS,
            nullptr, nullptr);
        SendMessageW(g_hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    }
    break;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case ID_BTN_INSTALL:
            EnableWindow(g_hBtnInstall, FALSE);
            EnableWindow(g_hBtnUpdate, FALSE);
            EnableWindow(g_hBtnUninstall, FALSE);
            std::thread([&] {
                DoInstall(g_hProgress);
                EnableWindow(g_hBtnInstall, TRUE);
                EnableWindow(g_hBtnUpdate, TRUE);
                EnableWindow(g_hBtnUninstall, TRUE);
                }).detach();
            break;

        case ID_BTN_UPDATE:
        {
            std::wstring remote;
            if (IsUpdateAvailable(remote))
            {
                int ans = MessageBoxW(hwnd,
                    (L"Доступна новая версия: " + remote + L"\nОбновить сейчас?").c_str(),
                    L"Обновление", MB_YESNO | MB_ICONQUESTION);
                if (ans == IDYES)
                {
                    std::wstring progFiles = GetKnownFolderPath(CSIDL_PROGRAM_FILES);
                    std::wstring installDir = progFiles + L"\\HIDE";
                    std::wstring jarPath = installDir + L"\\HIDE.jar";
                    DownloadFile(JAR_URL, jarPath);
                    MessageBoxW(hwnd,
                        L"Обновление завершено.", L"Info", MB_ICONINFORMATION);
                }
            }
            else
                MessageBoxW(hwnd,
                    L"Установлена самая свежая версия.", L"Info", MB_ICONINFORMATION);
        }
        break;

        case ID_BTN_UNINSTALL:
            if (MessageBoxW(hwnd,
                L"Вы действительно хотите полностью удалить HIDE?",
                L"Подтверждение", MB_YESNO | MB_ICONQUESTION) == IDYES)
            {
                PerformUninstall();
                MessageBoxW(hwnd,
                    L"Программа удалена.", L"Uninstall", MB_ICONINFORMATION);
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
            break;

        case ID_BTN_EXIT:
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            break;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ------------------------------------------------------------------
// Точка входа ---------------------------------------------------------

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE /*hPrevInstance*/,
    _In_ LPWSTR    lpCmdLine,
    _In_ int       nCmdShow)
{
    // Деинсталляция через параметр /uninstall
    if (wcsstr(lpCmdLine, L"/uninstall"))
    {
        if (PerformUninstall())
            MessageBoxW(nullptr,
                L"Программа успешно удалена.", L"Uninstall", MB_ICONINFORMATION);
        else
            MessageBoxW(nullptr,
                L"Не удалось выполнить полное удаление.", L"Uninstall", MB_ICONERROR);
        return 0;
    }

    // Иконка окна установщика (ресурсный файл)
    HICON hInstIcon = LoadIconW(hInstance, MAKEINTRESOURCE(IDI_APPICON));
    if (!hInstIcon)                     // fallback – системный значок
        hInstIcon = LoadIconW(nullptr, IDI_APPLICATION);

    const wchar_t CLASS_NAME[] = L"HIDE_INSTALLER_CLASS";

    // Регистрация окна
    WNDCLASSEXW wcex = { sizeof(WNDCLASSEXW) };
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hIcon = hInstIcon;
    wcex.hIconSm = hInstIcon;
    wcex.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszClassName = CLASS_NAME;

    RegisterClassExW(&wcex);

    HWND hwnd = CreateWindowExW(0,
        CLASS_NAME, L"HIDE Installer",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 460, 200,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) return 0;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
