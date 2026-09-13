#ifndef WINJECT_MANAGER_LOG_H_
#define WINJECT_MANAGER_LOG_H_

// Line format: YYYY-MM-DD HH:MM:SS.sss | LEVEL | msg
void log_printf(const char* level, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#define LOG_INF(fmt, ...) log_printf("INF", fmt, ##__VA_ARGS__)
#define LOG_ERR(fmt, ...) log_printf("ERR", fmt, ##__VA_ARGS__)
#define LOG_WRN(fmt, ...) log_printf("WRN", fmt, ##__VA_ARGS__)

#endif  // WINJECT_MANAGER_LOG_H_
