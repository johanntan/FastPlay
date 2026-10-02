// Running helper programs on Windows: CreateProcess with a pipe for the output.

#include "subprocess.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cwchar>

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
	std::string& output, std::string* errors, int* exitCode, const ProcessOptions& options) {
	output.clear();
	if (errors) errors->clear();
	if (exitCode) *exitCode = -1;

	std::wstring cmdLine = L"\"" + program + L"\"";
	for (const auto& arg : args) {
		cmdLine += L' ';
		AppendQuotedArg(cmdLine, arg);
	}

	std::vector<wchar_t> environment;
	if (!options.pathDirectories.empty()) {
		std::vector<std::wstring> entries;
		std::wstring path;
		LPWCH block = GetEnvironmentStringsW();
		if (!block) return false;
		for (const wchar_t* entry = block; *entry; entry += wcslen(entry) + 1) {
			if (_wcsnicmp(entry, L"PATH=", 5) == 0) path = entry + 5;
			else entries.emplace_back(entry);
		}
		FreeEnvironmentStringsW(block);
		for (const auto& dir : options.pathDirectories) {
			if (!path.empty()) path += L';';
			path += dir;
		}
		entries.push_back(L"PATH=" + path);
		std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
			return _wcsicmp(a.c_str(), b.c_str()) < 0;
		});
		for (const auto& entry : entries) {
			environment.insert(environment.end(), entry.begin(), entry.end());
			environment.push_back(0);
		}
		environment.push_back(0);
	}

	SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
	HANDLE outRead = nullptr, outWrite = nullptr;
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
	STARTUPINFOW si{};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	si.hStdInput = nul;
	si.hStdOutput = outWrite;
	si.hStdError = errWrite;
	si.wShowWindow = SW_HIDE;

	// Timed diagnostics run in a job so a timeout also stops helper processes.
	HANDLE job = nullptr;
	if (options.timeoutMs > 0) {
		job = CreateJobObjectW(nullptr, nullptr);
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
		limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
			if (job) CloseHandle(job);
			CloseHandle(outRead);
			CloseHandle(outWrite);
			if (errRead) CloseHandle(errRead);
			if (errWrite && errWrite != INVALID_HANDLE_VALUE) CloseHandle(errWrite);
			if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
			return false;
		}
	}
	PROCESS_INFORMATION pi{};
	DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | (job ? CREATE_SUSPENDED : 0);
	BOOL started = CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, TRUE, flags,
		environment.empty() ? nullptr : environment.data(), nullptr, &si, &pi);
	CloseHandle(outWrite);
	if (errWrite && errWrite != INVALID_HANDLE_VALUE) CloseHandle(errWrite);
	if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
	if (started && job) {
		if (!AssignProcessToJobObject(job, pi.hProcess)) {
			TerminateProcess(pi.hProcess, 1);
			WaitForSingleObject(pi.hProcess, INFINITE);
			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);
			started = FALSE;
		} else ResumeThread(pi.hThread);
	}
	if (!started) {
		CloseHandle(outRead);
		if (errRead) CloseHandle(errRead);
		if (job) CloseHandle(job);
		return false;
	}

	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.timeoutMs);
	bool timedOut = false;
	HANDLE pipes[2] = {outRead, errRead};
	std::string* sinks[2] = {&output, errors};
	bool open[2] = {true, errors != nullptr};
	char buffer[4096];
	while (open[0] || open[1] || WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT) {
		if (options.timeoutMs > 0 && std::chrono::steady_clock::now() >= deadline) {
			timedOut = true;
			TerminateJobObject(job, 1);
			break;
		}
		bool readData = false;
		for (int i = 0; i < 2; ++i) {
			if (!open[i]) continue;
			DWORD available = 0;
			if (!PeekNamedPipe(pipes[i], nullptr, 0, nullptr, &available, nullptr)) {
				open[i] = false;
				continue;
			}
			if (!available) continue;
			DWORD read = 0;
			if (ReadFile(pipes[i], buffer, std::min<DWORD>(available, sizeof(buffer)), &read, nullptr) && read) {
				sinks[i]->append(buffer, read);
				readData = true;
			} else open[i] = false;
		}
		if (!readData) Sleep(10);
	}
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 0;
	GetExitCodeProcess(pi.hProcess, &code);
	if (exitCode) *exitCode = timedOut ? -2 : static_cast<int>(code);
	CloseHandle(outRead);
	if (errRead) CloseHandle(errRead);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	if (job) CloseHandle(job);
	return true;
}
