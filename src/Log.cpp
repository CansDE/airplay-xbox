#include "Log.h"

#include <windows.h>

#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>

namespace app {

namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;

// Schreibt den aktuellen Aufrufstack als "Modul+Offset" ins Log (Offsets lassen sich mit der Linker-Map aufloesen).
void LogStack(const char* title) {
    void* frames[48];
    USHORT n = CaptureStackBackTrace(1, 48, frames, nullptr);
    Log("---- %s: Stack (%u Frames)", title, static_cast<unsigned>(n));
    for (USHORT i = 0; i < n; ++i) {
        HMODULE mod = nullptr;
        wchar_t name[MAX_PATH] = L"?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCWSTR>(frames[i]), &mod) &&
            mod) {
            GetModuleFileNameW(mod, name, MAX_PATH);
        }
        const wchar_t* base = wcsrchr(name, L'\\');
        base = base ? base + 1 : name;
        Log("   #%02u %ls+0x%llx", static_cast<unsigned>(i), base,
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(mod)));
    }
}

void InvalidParameterHandler(const wchar_t* expr, const wchar_t* func, const wchar_t* file, unsigned line, uintptr_t) {
    Log("ABSTURZ: ungueltiger CRT-Parameter  expr=%ls func=%ls file=%ls line=%u", expr ? expr : L"-",
        func ? func : L"-", file ? file : L"-", line);
    LogStack("invalid parameter");
    TerminateProcess(GetCurrentProcess(), 0xC0000409);
}

void AbortHandler(int) {
    Log("ABSTURZ: abort() / assert");
    LogStack("abort");
    TerminateProcess(GetCurrentProcess(), 0xC0000409);
}

void TerminateHandler() {
    Log("ABSTURZ: std::terminate (nicht abgefangene C++-Exception)");
    LogStack("terminate");
    TerminateProcess(GetCurrentProcess(), 0xC0000409);
}

LONG CALLBACK VectoredHandler(PEXCEPTION_POINTERS info) {
    DWORD code = info->ExceptionRecord->ExceptionCode;
    // Nur schwere Fehler protokollieren (C++-Exceptions 0xE06D7363 und Debugger-Meldungen ignorieren).
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_ARRAY_BOUNDS_EXCEEDED ||
        code == EXCEPTION_INVALID_HANDLE) {  // ungueltiges Handle (UWP prueft Handles strikt)
        Log("ABSTURZ: Exception 0x%08lX bei %p", code, info->ExceptionRecord->ExceptionAddress);
        LogStack("exception");
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

void InstallCrashDiagnostics() {
    _set_invalid_parameter_handler(InvalidParameterHandler);
    signal(SIGABRT, AbortHandler);
    std::set_terminate(TerminateHandler);
    AddVectoredExceptionHandler(1, VectoredHandler);
}

void LogInit(const std::wstring& folder) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) return;
    std::wstring path = folder + L"\\airplay.log";
    // Alte Logs begrenzen: ueber 2 MB neu anfangen.
    WIN32_FILE_ATTRIBUTE_DATA info{};
    const wchar_t* mode = L"a";
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) && info.nFileSizeLow > 2u * 1024 * 1024) {
        mode = L"w";
    }
    g_file = _wfopen(path.c_str(), mode);
}

void Log(const char* fmt, ...) {
    char msg[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg) - 2, fmt, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[2200];
    int n = snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d  %s\n", t.wHour, t.wMinute, t.wSecond,
                     t.wMilliseconds, msg);
    if (n <= 0) return;

    std::lock_guard<std::mutex> lock(g_mutex);
    OutputDebugStringA(line);
    if (g_file) {
        fwrite(line, 1, static_cast<size_t>(n), g_file);
        fflush(g_file);
    }
}

}  // namespace app
