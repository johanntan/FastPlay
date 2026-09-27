#pragma once
#ifndef FASTPLAY_UPDATER_INTERNAL_H
#define FASTPLAY_UPDATER_INTERNAL_H

// Between the shared updater (src/updater.cpp) and each system's ApplyUpdate
// (src/platform/updater_*.cpp).

#include <string>

// Where the downloaded update goes
std::wstring UpdateZipPath();
std::wstring UpdateInstallerPath();

// Whether the last download was the installer rather than the zip
bool UpdateWithInstaller();

#endif // FASTPLAY_UPDATER_INTERNAL_H
