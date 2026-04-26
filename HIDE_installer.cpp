/*
 *  HIDE Installer – исправленная версия
 *  ---------------------------------
 *  Основные изменения:
 *  • Поиск установленного JDK/JRE 21 в %ProgramFiles% (и %ProgramFiles(x86)%)
 *  • `IsJavaInstalled()` теперь проверяет найденный `javaw.exe`
 *  • В ярлыках используется полный путь к `javaw.exe`
 *  • Диалоговые сообщения исправлены на «Java 21`
 *  • При обновлении (кнопка «Обновить») удаляются HIDE + Java, затем
 *    заново устанавливается Java 21 и HIDE.
 */

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
#include <algorithm>
#include <vector>
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

const std::wstring JAVA_INSTALLER_URL = L"https://download.oracle.com/java/21/archive/jdk-21.0.10_windows-x64_bin.exe";

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
    // Если файл уже существует – не скачиваем заново
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
// Поиск установленного JDK/JRE 21

/*  Возвращает полный путь к javaw.exe, если в системе найден JDK/JRE 21.
    Ищет в %ProgramFiles% и %ProgramFiles(x86)% каталоги
    \Java\jdk-21*  и  \Java\jre-21*  (регистр не важен).
    Если ничего не найдено – возвращает пустую строку.                */
std::wstring FindJavaExecutable()
{
    std::vector<std::wstring> roots = {
        GetKnownFolderPath(CSIDL_PROGRAM_FILES),
        GetKnownFolderPath(CSIDL_PROGRAM_FILESX86)
    };

    for (const auto& root : roots) {
        if (root.empty()) continue;
        std::wstring javaRoot = root + L"\\Java";
        if (!PathFileExistsW(javaRoot.c_str())) continue;

        WIN32_FIND_DATAW ffd;
        HANDLE hFind = FindFirstFileW((javaRoot + L"\\*").c_str(), &ffd);
        if (hFind == INVALID_HANDLE_VALUE) continue;

        do {
            if (!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                continue;

            std::wstring name = ffd.cFileName;
            if (name == L"." || name == L"..")
                continue;

            std::wstring lname = name;
            std::transform(lname.begin(), lname.end(),
                lname.begin(),
                ::towlower);

            if (lname.find(L"jdk-21") != std::wstring::npos ||
                lname.find(L"jre-21") != std::wstring::npos)
            {
                std::wstring candidate = javaRoot + L"\\" + name + L"\\bin\\javaw.exe";
                if (PathFileExistsW(candidate.c_str()))
                {
                    FindClose(hFind);
                    return candidate;            // нашли!
                }
            }
        } while (FindNextFileW(hFind, &ffd));
        FindClose(hFind);
    }
    return L""; // не найдено
}

/*  Возвращает true, если удалось запустить javaw -version и получить код 0.
    Для обнаружения используется полный путь, полученный `FindJavaExecutable()`. */
bool IsJavaInstalled()
{
    std::wstring javaExe = FindJavaExecutable();
    if (javaExe.empty())
        return false;   // нет даже установленного JDK/JRE

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};

    std::wstring cmd = L"\"" + javaExe + L"\" -version";
    BOOL ok = CreateProcessW(nullptr,
        (LPWSTR)cmd.c_str(),
        nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi);
    if (!ok) return false;

    // Ждём максимум 5 секунд
    WaitForSingleObject(pi.hProcess, 5000);
    DWORD ec = 0;
    GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (ec == 0);
}

/*  Запуск установщика JDK‑21 (если URL задан).
    Возвращает true, если установщик успешно запущен и завершён.
    Установщик запускается от имени администратора.                      */
bool InstallJava()
{
    if (JAVA_INSTALLER_URL.empty())
        return false;

    WCHAR tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring installer = std::wstring(tmp) + L"java_installer.exe";

    if (!DownloadFile(JAVA_INSTALLER_URL, installer))
        return false;

    // Запускаем установщик с повышенными привилегиями.
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = installer.c_str();
    sei.lpParameters = nullptr;          // обычный (не тихий) UI
    sei.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei))
        return false;

    // Дождёмся завершения установщика
    WaitForSingleObject(sei.hProcess, INFINITE);
    CloseHandle(sei.hProcess);

    // Удалим временный файл‑установщик
    DeleteFileW(installer.c_str());
    return true;
}

// ------------------------------------------------------------------
// Удаление каталогов рекурсивно (нужен для Java)

bool DeleteDirectoryRecursive(const std::wstring& dir)
{
    std::wstring pattern = dir + L"\\*";
    WIN32_FIND_DATAW ffd;
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE) {
        // Папка может быть уже пустой
        return RemoveDirectoryW(dir.c_str()) != 0;
    }

    do {
        if (wcscmp(ffd.cFileName, L".") == 0 ||
            wcscmp(ffd.cFileName, L"..") == 0)
            continue;

        std::wstring fullPath = dir + L"\\" + ffd.cFileName;
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            DeleteDirectoryRecursive(fullPath);
        }
        else {
            SetFileAttributesW(fullPath.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(fullPath.c_str());
        }
    } while (FindNextFileW(hFind, &ffd));
    FindClose(hFind);
    return RemoveDirectoryW(dir.c_str()) != 0;
}

/*  Удаляем установленную Java (JDK/JRE) из %ProgramFiles%\Java
    и %ProgramFiles(x86)%\Java (если такие каталоги есть).           */
bool UninstallJava()
{
    std::vector<std::wstring> roots = {
        GetKnownFolderPath(CSIDL_PROGRAM_FILES),
        GetKnownFolderPath(CSIDL_PROGRAM_FILESX86)
    };
    bool anyRemoved = false;

    for (const auto& root : roots) {
        if (root.empty()) continue;
        std::wstring javaRoot = root + L"\\Java";
        if (!PathFileExistsW(javaRoot.c_str())) continue;

        WIN32_FIND_DATAW ffd;
        HANDLE hFind = FindFirstFileW((javaRoot + L"\\*").c_str(), &ffd);
        if (hFind == INVALID_HANDLE_VALUE) continue;

        do {
            if (!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                continue;

            std::wstring name = ffd.cFileName;
            if (name == L"." || name == L"..") continue;

            std::wstring lname = name;
            std::transform(lname.begin(), lname.end(),
                lname.begin(),
                ::towlower);

            if (lname.find(L"jdk-21") != std::wstring::npos ||
                lname.find(L"jre-21") != std::wstring::npos) {
                std::wstring fullPath = javaRoot + L"\\" + name;
                if (DeleteDirectoryRecursive(fullPath))
                    anyRemoved = true;
            }
        } while (FindNextFileW(hFind, &ffd));
        FindClose(hFind);
    }
    return anyRemoved;
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

// Принудительное «pin‑ит» в раздел «Pinned» меню Пуск (verb "startpin")
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

    // Снимаем «pin» (если пользователь уже навесил)
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
    DeleteFileW((installDir + L"\\icon.jpg").c_str());

    // Папка (если пустая)
    RemoveDirectoryW(installDir.c_str());

    // Убираем запись из реестра
    RegDeleteKeyW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\HIDE");

    return true;
}

// ------------------------------------------------------------------
// Основная операция установки

/*  Параметр `skipJavaPrompt`:
    * `false` – обычный режим установки; при отсутствии Java выводим запрос.
    * `true`  – режим «обновление», когда Java только что установлена
               (запрос не нужен).                                            */
bool DoInstall(HWND hProgress, bool skipJavaPrompt = false)
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

    // 3. Скачиваем JAR, icon.ico и *неиспользуемый* icon.jpg
    std::wstring jarPath = installDir + L"\\HIDE.jar";
    std::wstring icoPath = installDir + L"\\icon.ico";
    std::wstring jpgPath = installDir + L"\\icon.jpg";

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

    if (!DownloadFile(ICON_JPG_URL, jpgPath))
    {
        MessageBoxW(nullptr,
            L"Не удалось скачать icon.jpg (всё равно установимся).",
            L"Warning", MB_ICONWARNING);
        // Ошибку можно игнорировать.
    }
    SendMessageW(hProgress, PBM_SETPOS, 60, 0);

    // 4. Проверка/установка Java 21
    if (!IsJavaInstalled())
    {
        if (!skipJavaPrompt)
        {
            int rc = MessageBoxW(nullptr,
                L"Java 21 не найдена. Установить сейчас?",
                L"Java not found",
                MB_YESNO | MB_ICONQUESTION);
            if (rc != IDYES)
                return false;   // пользователь отказался
        }

        if (!InstallJava())
        {
            MessageBoxW(nullptr,
                L"Не удалось установить Java 21.", L"Error", MB_ICONERROR);
            return false;
        }

        // Даем системе шанс «распознать» только‑что установленную Java.
        // Если после установки всё равно не найдена – выводим сообщение.
        if (!IsJavaInstalled())
        {
            MessageBoxW(nullptr,
                L"Java 21 по‑прежнему не найдена после установки.",
                L"Error", MB_ICONERROR);
            return false;
        }
    }

    // 5. Путь к javaw.exe (полный, гарантирует работу)
    std::wstring javaExe = FindJavaExecutable();
    if (javaExe.empty())
        javaExe = L"javaw.exe";    // fallback – мало чем может помочь, но не упадём

    std::wstring args = L"-jar \"" + jarPath + L"\"";

    // 5.1 Ярлык на рабочем столе
    std::wstring desktop = GetKnownFolderPath(CSIDL_DESKTOPDIRECTORY);
    std::wstring desktopLnk = desktop + L"\\HIDE.lnk";
    CreateShortcut(javaExe, args, desktopLnk,
        DISPLAY_NAME, icoPath);

    // 5.2 Ярлык в меню Пуск (общий раздел)
    std::wstring startMenu = GetKnownFolderPath(CSIDL_COMMON_PROGRAMS) + L"\\HIDE";
    EnsureDirectory(startMenu);
    std::wstring startLnk = startMenu + L"\\HIDE.lnk";
    CreateShortcut(javaExe, args, startLnk,
        DISPLAY_NAME, icoPath);

    // 5.3 Ярлык деинсталлятора
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring uninstallLnk = startMenu + L"\\Uninstall HIDE.lnk";
    CreateShortcut(exePath, L"/uninstall", uninstallLnk,
        L"Uninstall " + DISPLAY_NAME, icoPath);

    // 5.4 Прикрепляем основной ярлык к панели задач
    PinToTaskbar(startLnk);

    // 5.5 Принудительно «pin‑им» в раздел «Pinned» меню Пуск
    PinToStartMenu(startLnk);

    // 6. Регистрация в «Программы и компоненты»
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
            std::thread([] {
                DoInstall(g_hProgress);
                EnableWindow(g_hBtnInstall, TRUE);
                EnableWindow(g_hBtnUpdate, TRUE);
                EnableWindow(g_hBtnUninstall, TRUE);
                }).detach();
            break;

        case ID_BTN_UPDATE:
        {
            // Отключаем кнопки – работа идёт в отдельном потоке
            EnableWindow(g_hBtnInstall, FALSE);
            EnableWindow(g_hBtnUpdate, FALSE);
            EnableWindow(g_hBtnUninstall, FALSE);

            std::thread([] {
                std::wstring remote;
                bool hasUpdate = IsUpdateAvailable(remote);
                std::wstring msg = hasUpdate
                    ? L"Доступна новая версия: " + remote + L".\n"
                    : L"Установлена самая свежая версия.\n";

                msg += L"Будет выполнена полная переустановка HIDE и Java 21.\nПродолжить?";

                int ans = MessageBoxW(nullptr,
                    msg.c_str(),
                    L"Обновление",
                    MB_YESNO | MB_ICONQUESTION);
                if (ans != IDYES) {
                    EnableWindow(g_hBtnInstall, TRUE);
                    EnableWindow(g_hBtnUpdate, TRUE);
                    EnableWindow(g_hBtnUninstall, TRUE);
                    return;
                }

                if (!IsRunningAsAdmin()) {
                    MessageBoxW(nullptr,
                        L"Для обновления требуются права администратора.",
                        L"Error", MB_ICONERROR);
                    EnableWindow(g_hBtnInstall, TRUE);
                    EnableWindow(g_hBtnUpdate, TRUE);
                    EnableWindow(g_hBtnUninstall, TRUE);
                    return;
                }

                // 1) Удаляем HIDE
                PerformUninstall();

                // 2) Удаляем установленную Java (если что‑то осталось)
                UninstallJava();

                // 3) Устанавливаем Java 21 (без диалогов)
                if (!InstallJava())
                {
                    MessageBoxW(nullptr,
                        L"Не удалось установить Java 21.",
                        L"Error", MB_ICONERROR);
                    EnableWindow(g_hBtnInstall, TRUE);
                    EnableWindow(g_hBtnUpdate, TRUE);
                    EnableWindow(g_hBtnUninstall, TRUE);
                    return;
                }

                // 4) Устанавливаем HIDE; запрос о Java не нужен – уже установлена
                DoInstall(g_hProgress, true);

                MessageBoxW(nullptr,
                    L"Обновление завершено.", L"Info", MB_ICONINFORMATION);

                EnableWindow(g_hBtnInstall, TRUE);
                EnableWindow(g_hBtnUpdate, TRUE);
                EnableWindow(g_hBtnUninstall, TRUE);
                }).detach();
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
