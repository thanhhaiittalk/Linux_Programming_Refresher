// src/shared_stats.hpp
#pragma once

#include <pthread.h>
#include <cstdint>
#include <sys/types.h>

static constexpr const char* SHM_STATS_NAME = "/logd.stats";

struct SharedStats {
    pthread_mutex_t lock; // must be initialized with PTHREAD_PROCESS_SHARED
    uint64_t        value; // dummy data
    pid_t           writer; // pid of last writer
};
static_assert(sizeof(SharedStats) <= 1024 * 16, "SharedStats too large"); // sanity check
