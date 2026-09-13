#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

void log_printf(const char* level, const char* fmt, ...)
{
    timespec ts = {};
    clock_gettime(CLOCK_REALTIME, &ts);
    tm local = {};
    localtime_r(&ts.tv_sec, &local);

    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
    const int ms = static_cast<int>(ts.tv_nsec / 1000000);

    flockfile(stderr);
    fprintf(stderr, "%s.%03d | %s | ", stamp, ms, level);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    funlockfile(stderr);
}
