#include "hotkeys.h"
#include "ini.h"
#include "globals.h"
#include "types.h"
#include <cstdio>

// Format hotkey for display (e.g., "Ctrl+Shift+P")
std::wstring FormatHotkey(UINT modifiers, UINT vk) {
    std::wstring result;
    if (modifiers & MOD_CONTROL) result += L"Ctrl+";
    if (modifiers & MOD_ALT) result += L"Alt+";
    if (modifiers & MOD_SHIFT) result += L"Shift+";
    if (modifiers & MOD_WIN) result += L"Win+";

    // Get key name
    UINT scanCode = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG keyParam = static_cast<LONG>(scanCode << 16);
    // Keys on the extended part of the keyboard share scan codes with the numeric
    // keypad; without the extended bit, Up would be named "Num 8".
    switch (vk) {
        case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
        case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
        case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
        case VK_RCONTROL: case VK_RMENU: case VK_LWIN: case VK_RWIN: case VK_APPS:
            keyParam |= 1 << 24;
            break;
    }
    wchar_t keyName[64] = {0};
    GetKeyNameTextW(keyParam, keyName, 64);
    if (keyName[0]) {
        result += keyName;
    } else {
        wchar_t buf[16];
        swprintf(buf, 16, L"0x%02X", vk);
        result += buf;
    }
    return result;
}

// Load hotkeys from INI file
void LoadHotkeys() {
    g_hotkeysEnabled = IniGetInt(L"Hotkeys", L"Enabled", 1, g_configPath.c_str()) != 0;
    g_hotkeys.clear();
    int count = IniGetInt(L"Hotkeys", L"Count", 0, g_configPath.c_str());

    for (int i = 0; i < count; i++) {
        wchar_t key[32];
        wchar_t value[64] = {0};

        swprintf(key, 32, L"Hotkey%d", i);
        IniGetString(L"Hotkeys", key, L"", value, 64, g_configPath.c_str());

        // Parse "modifiers,vk,actionIdx"
        UINT mods = 0, vk = 0;
        int actionIdx = 0;
        if (swscanf(value, L"%u,%u,%d", &mods, &vk, &actionIdx) == 3) {
            if (actionIdx >= 0 && actionIdx < g_hotkeyActionCount) {
                GlobalHotkey hk;
                hk.id = g_nextHotkeyId++;
                hk.modifiers = mods;
                hk.vk = vk;
                hk.actionIdx = actionIdx;
                g_hotkeys.push_back(hk);
            }
        }
    }
}

// Save hotkeys to INI file
void SaveHotkeys() {
    wchar_t buf[64];

    IniWriteString(L"Hotkeys", L"Enabled", g_hotkeysEnabled ? L"1" : L"0", g_configPath.c_str());

    swprintf(buf, 64, L"%d", static_cast<int>(g_hotkeys.size()));
    IniWriteString(L"Hotkeys", L"Count", buf, g_configPath.c_str());

    for (size_t i = 0; i < g_hotkeys.size(); i++) {
        wchar_t key[32];
        swprintf(key, 32, L"Hotkey%zu", i);
        swprintf(buf, 64, L"%u,%u,%d", g_hotkeys[i].modifiers, g_hotkeys[i].vk, g_hotkeys[i].actionIdx);
        IniWriteString(L"Hotkeys", key, buf, g_configPath.c_str());
    }
}

