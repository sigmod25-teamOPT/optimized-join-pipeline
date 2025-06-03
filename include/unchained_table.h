#pragma once
// implementation described by https://db.in.tum.de/~birler/papers/hashtable.pdf
// and https://cedardb.com/blog/simple_efficient_hash_tables/
#include <defs.h>
#include <vector>
#include <data_page.h>
#include <strings.h>
#include <iostream>
#include <partition.h>

extern const uint16_t tags[1 << 11];

template <typename T>
class unchained_table {
    public:
        unchained_table() : tupleStorage(nullptr), directory(nullptr), elements(0) {}

        ~unchained_table() {
            if (tupleStorage) delete[] tupleStorage;
            uint64_t* originalDirectory = directory - 1;
            if (directory) delete[] originalDirectory;
        }

        // Delete copy constructor and assignment operator
        unchained_table(const unchained_table&) = delete;
        unchained_table& operator=(const unchained_table&) = delete;

        void reserve(size_t capacity) {
            // Find the next power of 2 for capacity
            shift = 10; // lowest bound 1024
            while ((1 << shift) <= capacity) shift++;
            this->capacity = 1ull << shift;
            tupleStorage = new Entry<T>[this->capacity]();
            directory = new uint64_t[this->capacity + 1]();
            directory[0] = (uint64_t)tupleStorage << 16;
            ++directory;
            shift = 64 - shift;
        }

        void insert(Partitions& partitionTuples, const uint64_t partition, const uint64_t prevCount,
                    const uint64_t num_partitions) {
            elements += partitionTuples.counts[partition];
            size_t page_count = partitionTuples[partition].size();
            for(size_t i = 0; i < page_count; i++) {
                uint32_t row_count = partitionTuples[partition][i].get_size();
                for(size_t j = 0; j < row_count; j++) {
                    PartitionEntry<T> entry = partitionTuples[partition][i].get<PartitionEntry<T>>(j);
                    uint64_t slot = entry.hash >> shift;
                    directory[slot] += sizeof(Entry<T>) << 16;
                    directory[slot] |= computeTag(entry.hash);
                }
            }
            // prevCount is the total tuple count of previous partitions
            uint64_t cur = (uint64_t)(tupleStorage + prevCount);
            uint64_t k = 64 - shift;
            uint64_t start = (partition << k) / num_partitions;
            uint64_t end = ((partition + 1) << k) / num_partitions;
            for (uint64_t i = start; i < end; i++) {
                uint64_t val = directory[i] >> 16;
                directory[i] = (cur << 16) | ((uint16_t)directory[i]);
                cur += val;
            }

            size_t rows = 0;
            for(size_t i = 0; i < page_count; i++) {
                uint32_t row_count = partitionTuples[partition][i].get_size();
                for(size_t j = 0; j < row_count; j++) {
                    PartitionEntry<T> entry = partitionTuples[partition][i].get<PartitionEntry<T>>(j);
                    uint64_t slot = entry.hash >> shift;
                    Entry<T>* target = (Entry<T>*)(directory[slot] >> 16);
                    *target = entry.entry;
                    directory[slot] += sizeof(Entry<T>) << 16;
                    rows++;
                }
            }
        }

        Entry<T>* lookup(const T& key, size_t& len) const {
            uint64_t hashv = unchained_table<T>::hash(key);
            uint64_t slot = hashv >> shift;
            uint64_t entry = directory[slot];
            if (!couldContain((uint16_t)entry, hashv)) return nullptr;
            Entry<T>* start = (Entry<T>*)(directory[slot - 1] >> 16);
            Entry<T>* end = (Entry<T>*)(entry >> 16);
            len = end - start;
            return start;
        }

        bool couldContain(const uint16_t entry, const uint64_t hash) const {
            uint16_t slot = ((uint32_t)hash) >> (32 - 11);
            uint16_t tag = tags[slot];
            return !(tag & ~entry);
        }

        static uint64_t hash(const T& key);

        int size() const {
            return elements;
        }
        bool empty() const;
    private:
        uint16_t computeTag(const uint64_t hash) const {
            return tags[((uint32_t)hash) >> (32 - 11)];
        }

        Entry<T>* tupleStorage;
        uint64_t* directory;
        uint64_t shift;
        uint64_t capacity;
        uint64_t elements;
};