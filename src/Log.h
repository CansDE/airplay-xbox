#pragma once
#include <string>

namespace app {

// Writes to OutputDebugString and (if set) to <folder>\airplay.log.
void LogInit(const std::wstring& folder);
void Log(const char* fmt, ...);

// Logs crashes (invalid CRT parameters, abort, exceptions) including the call stack.
void InstallCrashDiagnostics();

}  // namespace app
