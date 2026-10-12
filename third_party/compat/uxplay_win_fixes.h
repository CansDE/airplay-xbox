/* Force-included (/FI) into every UxPlay source file: fills the gaps between POSIX and MSVC/UWP. */
#ifndef UXPLAY_WIN_FIXES_H
#define UXPLAY_WIN_FIXES_H
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
typedef SSIZE_T ssize_t;
#ifndef SOL_TCP
#define SOL_TCP IPPROTO_TCP
#endif
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#endif
