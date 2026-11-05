/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Starts the installer with the Python that comes with it, from the folder this program is in. */
#include <wchar.h>
#include <windows.h>

#define COMMAND_CAPACITY 8192
#define WINDOW_TITLE L"Ben 10: Protector of Earth"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR arguments, int show) {
    (void)instance;
    (void)previous;
    (void)show;
    wchar_t folder[MAX_PATH];
    const DWORD length = GetModuleFileNameW(NULL, folder, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return 1;
    wchar_t *slash = wcsrchr(folder, L'\\');
    if (slash) *slash = L'\0';
    if (!SetCurrentDirectoryW(folder)) return 1;

    if (GetFileAttributesW(L"python\\pythonw.exe") == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(L"installer\\install.py") == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(NULL,
                    L"Extract the whole ZIP to a folder first, then open this program from there.\n\n"
                    L"Extraia o ZIP inteiro para uma pasta primeiro e abra este programa de lá.",
                    WINDOW_TITLE, MB_OK | MB_ICONINFORMATION);
        return 1;
    }

    static wchar_t command[COMMAND_CAPACITY] = L"python\\pythonw.exe -X utf8 installer\\install.py ";
    if (wcslen(command) + wcslen(arguments) + 1 >= COMMAND_CAPACITY) return 1;
    wcscat(command, arguments);
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process;
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
        MessageBoxW(NULL, L"The installer could not start. / O instalador não pôde iniciar.", WINDOW_TITLE,
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return 0;
}
