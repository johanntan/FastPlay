#pragma once
#ifndef FASTPLAY_SUBPROCESS_H
#define FASTPLAY_SUBPROCESS_H

// Running helper programs (src/platform/subprocess_*.cpp).

#include <string>
#include <vector>

struct ProcessOptions {
	int timeoutMs = 0; // Zero preserves the normal, unbounded wait. Timeout exit code is -2.
	std::vector<std::wstring> pathDirectories; // Appended to the child's PATH only.
};

// Run a program with the given arguments, hidden, and wait for it to finish. What it
// writes to stdout goes to `output` and what it writes to stderr to `errors` (raw
// bytes, normally UTF-8); stderr is discarded when `errors` is null. Each argument
// reaches the program as-is; no shell is involved. Returns false if the program
// could not be started; `exitCode` (if given) receives its exit code.
bool RunProcessCapture(const std::wstring& program, const std::vector<std::wstring>& args,
                       std::string& output, std::string* errors = nullptr, int* exitCode = nullptr,
	const ProcessOptions& options = {});

#endif // FASTPLAY_SUBPROCESS_H
