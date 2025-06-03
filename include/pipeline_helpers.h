#pragma once

#include <atomic>
#include <memory>
#include <cassert>
#include <hardware_parse.h>

enum PipelineState {
    DEFAULT = 0,
    PARTITIONING,
    BUILDING,
    WAIT_COLUMNS,
    SPAWNING,
    JOINING,
    FINISHED
};

static inline bool try_advance_state(std::atomic<PipelineState>& state, PipelineState to) {
    PipelineState old = state.load(std::memory_order_relaxed);
    if (old >= to) return false; /* Someone exchanged before me */
    /* Race to get the advancement, if someone is faster then it will return false because old is not atomic's value */
    bool success = state.compare_exchange_strong(old, to, std::memory_order_release, std::memory_order_relaxed);
    assert(!success || old < to);
    return success;
}

struct alignas(SPC__LEVEL1_DCACHE_LINESIZE) ColumnarTableWrapper {
    ColumnarTable table;
};