#pragma once
#include <string>

namespace app {

// Schreibt nach OutputDebugString und (wenn gesetzt) in <folder>\airplay.log.
void LogInit(const std::wstring& folder);
void Log(const char* fmt, ...);

// Protokolliert Abstuerze (ungueltige CRT-Parameter, abort, Exceptions) samt Aufrufstack ins Log.
void InstallCrashDiagnostics();

}  // namespace app
