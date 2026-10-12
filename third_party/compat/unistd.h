/* unistd.h-Ersatz fuer UWP: nur das, was UxPlay braucht. */
#ifndef COMPAT_UNISTD_H
#define COMPAT_UNISTD_H
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <io.h>
#include <stdlib.h>
#define usleep(x) Sleep(((x) + 999) / 1000)
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#endif
