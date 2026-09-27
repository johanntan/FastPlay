#pragma once
#ifndef FASTPLAY_SUBPROCESS_H
#define FASTPLAY_SUBPROCESS_H

// Running helper programs (src/platform/subprocess_*.cpp).

#include <string>
#include <vector>

// Run a program with the given arguments, hidden, and return everything it wrote to
// stdout and stderr (raw bytes, normally UTF-8). Each argument reaches the program
// as-is; no shell is involved. Returns false if the program could not be started.
bool RunProcessCapture(const std::wstring& program, const std::vector<std::wstring>& args,
                       std::string& output);

#endif // FASTPLAY_SUBPROCESS_H
