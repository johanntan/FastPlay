// Running helper programs on Windows: CreateProcess with a pipe for the output.

#include "subprocess.h"

#include <windows.h>

namespace {

// Quote one argument so CommandLineToArgvW (and the C runtime) reads it back unchanged.
void AppendQuotedArg(std::wstring& cmdLine, const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        cmdLine += arg;
        return;
    }
    cmdLine += L'"';
    for (size_t i = 0;; ++i) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++backslashes;
            ++i;
        }
        if (i == arg.size()) {
            // Backslashes before the closing quote are doubled.
            cmdLine.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            cmdLine.append(backslashes * 2 + 1, L'\\');
        } else {
            cmdLine.append(backslashes, L'\\');
        }
        cmdLine += arg[i];
    }
    cmdLine += L'"';
}

}  // namespace

bool RunProcessCapture(const std::wstring& program, const std::vector<std::wstring>& args,
                       std::string& output) {
    output.clear();

    std::wstring cmdLine = L"\"" + program + L"\"";
    for (const auto& arg : args) {
        cmdLine += L' ';
        AppendQuotedArg(cmdLine, arg);
    }

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) return false;

    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    if (!CreateProcessW(nullptr, &cmdLine[0], nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return false;
    }

    CloseHandle(hWritePipe);

    char buffer[4096];
    DWORD bytesRead;
    while (ReadFile(hReadPipe, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
        output.append(buffer, bytesRead);
    }

    CloseHandle(hReadPipe);
    WaitForSingleObject(pi.hProcess, 30000);  // 30 second timeout
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}
