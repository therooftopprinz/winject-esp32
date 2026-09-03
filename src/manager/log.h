#ifndef WINJECT_MANAGER_LOG_H_
#define WINJECT_MANAGER_LOG_H_

#include <cstdio>

#define LOG_INF(fmt, ...) \
    std::fprintf(stderr, "INF | " fmt "\n", ##__VA_ARGS__)
#define LOG_ERR(fmt, ...) \
    std::fprintf(stderr, "ERR | " fmt "\n", ##__VA_ARGS__)
#define LOG_WRN(fmt, ...) \
    std::fprintf(stderr, "WRN | " fmt "\n", ##__VA_ARGS__)

#endif  // WINJECT_MANAGER_LOG_H_
