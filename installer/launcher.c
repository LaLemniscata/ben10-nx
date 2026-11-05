/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Starts the installer with the Python that comes with it, from the folder this program is in. */
#include <stdio.h>
#include <wchar.h>
#include <windows.h>

#define COMMAND_CAPACITY 8192

int wmain(int argc, wchar_t **argv) {
    wchar_t folder[MAX_PATH];
    const DWORD length = GetModuleFileNameW(NULL, folder, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return 1;
    wchar_t *slash = wcsrchr(folder, L'\\');
    if (slash) *slash = L'\0';
    if (!SetCurrentDirectoryW(folder)) return 1;
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"Ben 10: Protector of Earth - installer");

    static wchar_t command[COMMAND_CAPACITY] = L"python\\python.exe -X utf8 installer\\install.py";
    for (int i = 1; i < argc; i++) {
        if (wcslen(command) + wcslen(argv[i]) + 4 >= COMMAND_CAPACITY) return 1;
        wcscat(command, L" \"");
        wcscat(command, argv[i]);
        wcscat(command, L"\"");
    }

    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process;
    DWORD result = 1;
    if (CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
        WaitForSingleObject(process.hProcess, INFINITE);
        GetExitCodeProcess(process.hProcess, &result);
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
    } else {
        fprintf(stderr, "Could not start the installer's Python (error %lu).\n", GetLastError());
    }
    fputs("\nPress Enter to close this window / Aperte Enter para fechar esta janela...", stdout);
    fflush(stdout);
    getchar();
    return (int)result;
}
