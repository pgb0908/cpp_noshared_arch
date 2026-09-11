#pragma once

#include <cstring>
#include <iostream>
#include <pthread.h>

inline void pin_thread_to_cpu(int cpu) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);

    const int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        std::cerr << "pin_thread_to_cpu(" << cpu << ") failed: " << std::strerror(rc)
                   << " -- continuing unpinned\n";
    }
}
