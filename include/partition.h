#pragma once

#include <cassert>
#include <table.h>
#include <stddef.h>
#include <data_page.h>
#include <iostream>
#include <unchained_table.h>

template <typename T>
struct Entry {
    T key;
    super_id row_id;
};

template <typename T>
struct PartitionEntry {
    struct Entry<T> entry;
    uint64_t hash;
};

struct alignas(SPC__LEVEL1_DCACHE_LINESIZE) Partitions {
    public:
        Partitions(DataType type, size_t num_parts = 1): num_rows(0), num_parts(num_parts), counts(num_parts, 0) {
            pages.resize(num_parts);
        }

        Partitions(Partitions&& o) noexcept
                : pages(std::move(o.pages)), counts(std::move(o.counts)), num_rows(o.num_rows), num_parts(o.num_parts) {
            if (this == &o) return;
            o.pages.clear();
            o.counts.clear();
            o.num_rows = 0;
            #ifndef NDEBUG
            do_not_use = o.do_not_use;
            #endif
        }

        Partitions& operator=(Partitions&& o) {
            if (this == &o) return *this;
            pages = std::move(o.pages);
            counts = std::move(o.counts);
            num_rows = o.num_rows;
            num_parts = o.num_parts;
            o.pages.clear();
            o.counts.clear();
            o.num_rows = 0;
            #ifndef NDEBUG
            do_not_use = o.do_not_use;
            #endif
            return *this;
        }

        Partitions(const Partitions& o) = delete;
        Partitions& operator=(const Partitions&) = delete;

        ~Partitions() {
            pages.clear();
        }

        void resize(size_t num_parts) {
            assert(num_rows == 0);
            pages.resize(num_parts);
            counts.resize(num_parts, 0);
            num_rows = 0;
            this->num_parts = num_parts;
        }

        template <typename T>
        void insert(size_t part_idx, PartitionEntry<T> value){
            assert(!do_not_use);
            if (pages[part_idx].empty() || pages[part_idx].back().full())
                pages[part_idx].emplace_back(sizeof(PartitionEntry<T>));

            pages[part_idx].back().insert<PartitionEntry<T>>(value);
            counts[part_idx]++;
            num_rows++;
        }

        void concat(Partitions& other) {
            assert(num_parts == other.num_parts);

            for(int i = 0; i < num_parts; i++){
                append_vectors(pages[i], other.pages[i]);
                counts[i] += other.counts[i];
            }
            num_rows += other.num_rows;
        }

        size_t size() const {
            return num_rows;
        }

        std::vector<DataPage>& operator[](size_t idx) {
            return pages[idx];
        }

    public:
        std::vector<std::vector<DataPage>> pages;
        std::vector<size_t> counts;
        size_t num_rows, num_parts;
        #ifndef NDEBUG
        bool do_not_use = false;
        #endif
};
