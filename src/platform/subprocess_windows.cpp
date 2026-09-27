// Running helper programs on Windows: CreateProcess with a pipe for the output.

#include "subprocess.h"

#include <windows.h>

#include <thread>

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
                       std::string& output, std::string* errors, int* exitCode) {
    output.clear();
    if (errors) errors->clear();

    std::wstring cmdLine = L"\"" + program + L"\"";
    for (const auto& arg : args) {
        cmdLine += L' ';
        AppendQuotedArg(cmdLine, arg);
    }

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    // stdout, and stderr either to its own pipe or to NUL
    HANDLE outRead, outWrite;
    if (!CreatePipe(&outRead, &outWrite, &sa, 0)) return false;
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

    HANDLE errRead = nullptr, errWrite = nullptr;
    if (errors) {
        if (!CreatePipe(&errRead, &errWrite, &sa, 0)) {
            CloseHandle(outRead);
            CloseHandle(outWrite);
            return false;
        }
        SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);
    } else {
        errWrite = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    }
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdInput = nul;
    si.hStdOutput = outWrite;
    si.hStdError = errWrite;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    BOOL started = CreateProcessW(nullptr, &cmdLine[0], nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    // The child has its own copies now (or never started).
    CloseHandle(outWrite);
    if (errWrite && errWrite != INVALID_HANDLE_VALUE) CloseHandle(errWrite);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!started) {
        CloseHandle(outRead);
        if (errRead) CloseHandle(errRead);
        return false;
    }

    auto drain = [](HANDLE pipe, std::string& into) {
        char buffer[4096];
        DWORD bytesRead;
        while (ReadFile(pipe, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
            into.append(buffer, bytesRead);
        }
    };
    // Both pipes are read at once, so neither can fill up and stall the program.
    std::thread errorReader;
    if (errRead) errorReader = std::thread(drain, errRead, std::ref(*errors));
    drain(outRead, output);
    if (errorReader.joinable()) errorReader.join();

    CloseHandle(outRead);
    if (errRead) CloseHandle(errRead);
    WaitForSingleObject(pi.hProcess, 30000);  // 30 second timeout
    if (exitCode) {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        *exitCode = static_cast<int>(code);
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}
