#pragma once
#include <windows.h>

namespace app {

// On-screen text language: German if the system UI language is German, English otherwise.
inline bool UiGerman() {
    static const bool german = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_GERMAN;
    return german;
}

inline const wchar_t* Tr(const wchar_t* de, const wchar_t* en) { return UiGerman() ? de : en; }
inline const char* Tr(const char* de, const char* en) { return UiGerman() ? de : en; }

}  // namespace app
