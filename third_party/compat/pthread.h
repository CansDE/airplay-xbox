/* Minimal Win32-based pthread replacement (UWP-compatible) for the UxPlay library. */
#ifndef COMPAT_PTHREAD_H
#define COMPAT_PTHREAD_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <process.h>
#include <time.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pthread_compat {
    HANDLE handle;
    void *(*func)(void *);
    void *arg;
} *pthread_t;
typedef CRITICAL_SECTION pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif

static __inline int clock_gettime(int clk, struct timespec *ts)
{
    FILETIME ft;
    ULARGE_INTEGER u;
    (void) clk;
    GetSystemTimePreciseAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart -= 116444736000000000ULL; /* 1601 -> 1970 */
    ts->tv_sec = (time_t) (u.QuadPart / 10000000ULL);
    ts->tv_nsec = (long) ((u.QuadPart % 10000000ULL) * 100ULL);
    return 0;
}

static unsigned __stdcall pthread_compat_start(void *p)
{
    struct pthread_compat *t = (struct pthread_compat *) p;
    t->func(t->arg);
    return 0;
}

static __inline int pthread_create(pthread_t *out, const void *attr, void *(*func)(void *), void *arg)
{
    struct pthread_compat *t = (struct pthread_compat *) malloc(sizeof(*t));
    (void) attr;
    if (!t) return -1;
    t->func = func;
    t->arg = arg;
    t->handle = (HANDLE) _beginthreadex(NULL, 0, pthread_compat_start, t, 0, NULL);
    if (!t->handle) { free(t); return -1; }
    *out = t;
    return 0;
}

static __inline int pthread_join(pthread_t t, void **ret)
{
    (void) ret;
    if (!t) return -1;
    WaitForSingleObject(t->handle, INFINITE);
    CloseHandle(t->handle);
    free(t);
    return 0;
}

static __inline int pthread_mutex_init(pthread_mutex_t *m, const void *a) { (void) a; InitializeCriticalSectionEx(m, 0, 0); return 0; }
static __inline int pthread_mutex_lock(pthread_mutex_t *m) { EnterCriticalSection(m); return 0; }
static __inline int pthread_mutex_unlock(pthread_mutex_t *m) { LeaveCriticalSection(m); return 0; }
static __inline int pthread_mutex_destroy(pthread_mutex_t *m) { DeleteCriticalSection(m); return 0; }

static __inline int pthread_cond_init(pthread_cond_t *c, const void *a) { (void) a; InitializeConditionVariable(c); return 0; }
static __inline int pthread_cond_signal(pthread_cond_t *c) { WakeConditionVariable(c); return 0; }
static __inline int pthread_cond_destroy(pthread_cond_t *c) { (void) c; return 0; }

/* abs: absolute CLOCK_REALTIME time */
static __inline int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abs)
{
    struct timespec now;
    long long ms;
    clock_gettime(CLOCK_REALTIME, &now);
    ms = ((long long) abs->tv_sec - now.tv_sec) * 1000LL + (abs->tv_nsec - now.tv_nsec) / 1000000LL;
    if (ms < 0) ms = 0;
    return SleepConditionVariableCS(c, m, (DWORD) ms) ? 0 : 110 /* ETIMEDOUT */;
}

#ifdef __cplusplus
}
#endif
#endif
