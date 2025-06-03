#pragma once

#include <data_column.h>
#include <inter_column.h>
#include <defs.h>
#include <vector>
#include <inter_result.h>
#include <pages_feeder.h>

struct alignas(SPC__LEVEL1_DCACHE_LINESIZE) JoinResult {
    // Constructor for ProbingTasks
    JoinResult(size_t width, DataType type) : result_rowids(width), result_col_join(type),
        row_count(0), from_scan(false), temporary(false) {}

    JoinResult(DataColumn data_col) : result_col_scan(data_col), row_count(data_col.size()), from_scan(true), temporary(false) {}

    JoinResult(int dummy) : temporary(true) {}

    JoinResult() : row_count(0) {
        #ifndef NDEBUG
        dont_use = true;
        #endif
    }

    ~JoinResult() {
        if (pages_feeders) delete[] pages_feeders;
    }


    JoinResult(const JoinResult&) = delete;
    JoinResult& operator=(const JoinResult&) = delete;

    JoinResult(JoinResult&& other) noexcept
        : result_rowids(std::move(other.result_rowids)),
          result_col_join(std::move(other.result_col_join)),
          result_col_scan(std::move(other.result_col_scan)),
          row_count(other.row_count),
          from_scan(other.from_scan),
          temporary(other.temporary)
    {
        #ifndef NDEBUG
        dont_use = other.dont_use;
        other.dont_use = true;
        #endif
    }

    JoinResult& operator=(JoinResult&& other) noexcept {
        if (this != &other) {
            result_rowids = std::move(other.result_rowids);
            result_col_join = std::move(other.result_col_join);
            result_col_scan = std::move(other.result_col_scan);
            row_count = other.row_count;
            from_scan = other.from_scan;
            temporary = other.temporary;

            #ifndef NDEBUG
            dont_use = other.dont_use;
            other.dont_use = true;
            #endif
        }
        return *this;
    }

    static void append_results(JoinResult& a, JoinResult b) {
        if(b.temporary) return;

        if(b.is_from_scan() || a.temporary)
            a = std::move(b);
        else {
            assert(!a.temporary);

            if(b.get_output_size() > 0) {
                assert(b.result_rowids.size() == b.result_col_join.size());
                assert(b.result_rowids.pages.size() == b.result_col_join.pages.size());
                a.result_rowids.concat(b.result_rowids);
                a.result_col_join.concat(b.result_col_join);
                assert(a.result_rowids.pages.size() == a.result_col_join.pages.size());
            }
        }
    }

    inline size_t get_output_size() const {
        assert(!dont_use);
        if(temporary) return 0;
        return is_from_scan() ? row_count : result_rowids.size();
    }

    bool is_from_scan() const {
        assert(!dont_use && !temporary);
        return from_scan;
    }

    bool is_empty() const {
        return get_output_size() == 0;
    }

    bool is_temporary() const {
        return temporary;
    }

    bool get_next_page(size_t& out_page, size_t num_pages) {
        assert(!dont_use);
        assert(num_pages != -1ull);

        auto old = page_idx.load(std::memory_order_relaxed);
        while (!page_idx.compare_exchange_weak(old, old + 1, std::memory_order_release, std::memory_order_relaxed));
        auto next = old;

        if(next >= num_pages)
            return false;

        out_page = next;
        assert(out_page < num_pages);
        return true;
    }

    IntermediateResult result_rowids;
    IntermediateColumn result_col_join;
    DataColumn result_col_scan;

    size_t row_count;
    PagesFeeder* pages_feeders = nullptr;
    std::atomic<size_t> page_idx = {0};

private:
    bool from_scan;
    bool temporary;

    #ifndef NDEBUG
    bool dont_use = false;
    #endif
};

class JoinMetadata {
    public:
        JoinMetadata() = default;

        JoinMetadata(int scan_id, std::vector<ColumnElement> col_seq) {
            scan_id_to_col.push_back(scan_id);
            this->col_seq = std::move(col_seq);
        }

        JoinMetadata(std::vector<ColumnElement> col_seq, std::vector<uint8_t> scan_id_to_col)
            : col_seq(col_seq), scan_id_to_col(scan_id_to_col) {}

        size_t get_column_index_in_results(size_t join_column) const {
            assert(join_column < col_seq.size());

            uint8_t target_scan_id = col_seq[join_column].scan_id;

            for(size_t i = 0; i < scan_id_to_col.size(); i++)
                if(scan_id_to_col[i] == target_scan_id)
                    return i;

            assert(false);
            return -1ull;
        }

        ColumnElement get_column_element(size_t join_column_id) const {
            assert(join_column_id < col_seq.size());
            return col_seq[join_column_id];
        }

        std::vector<ColumnElement> col_seq;
        std::vector<uint8_t> scan_id_to_col;
};
