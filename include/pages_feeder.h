#pragma once

#include <cstddef>
#include <atomic>
#include <hardware_parse.h>

struct alignas(SPC__LEVEL1_DCACHE_LINESIZE) PagesFeeder {
    PagesFeeder() = default;
    void set_param(size_t start, size_t count) {
        end_page = start + count;
        current_page = start;
    }

    ~PagesFeeder() = default;
    PagesFeeder(const PagesFeeder&) = delete;
    PagesFeeder& operator=(const PagesFeeder&) = delete;
    PagesFeeder(PagesFeeder&&) = delete;
    PagesFeeder& operator=(PagesFeeder&&) = delete;

    size_t next() {
        if (done) return -1ull;
        size_t old = current_page.load(std::memory_order_relaxed);
        while (!current_page.compare_exchange_weak(old, old + 1, std::memory_order_relaxed, std::memory_order_relaxed));
        if (old >= end_page) {
            done = true;
            return -1ull;
        }
        return old;
    }
    size_t end_page;
    std::atomic<size_t> current_page = {-1ull};
    bool done = false;
};

