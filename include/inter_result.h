#pragma once

#include <utility>
#include <cstddef>
#include <string.h>
#include <utils.h>
#include <cassert>
#include <data_page.h>
#include <defs.h>

struct rid_vec {
    rid_t* data;
    size_t size;
};

class IntermediateResult {
    public:
        // Start with 1 page
        IntermediateResult(size_t width=0) : width(width), num_rows(0) {
            pages.reserve(4096);
        }

        rid_vec get_row(super_id idx) const {
            size_t page_idx = idx.offset.page;
            size_t row_idx = idx.offset.offset;
            return {.data = (rid_t*)pages[page_idx].get_ptr(row_idx), .size = width};
        }

        rid_vec operator[](super_id idx) const {
            return get_row(idx);
        }

        size_t size() const {
            return num_rows;
        }

        void add_cat_rows(rid_vec left, rid_vec right) { // concat 2 rows to result
            assert(left.size + right.size == width);
            if (pages.empty() || pages.back().full()) {
                pages.emplace_back(width * sizeof(rid_t));
            }
            pages.back().insert_parts(left.data, left.size, right.data, right.size);
            num_rows++;
        }

        void concat(const IntermediateResult& other) {
            assert(width == other.width);
            if(pages.size() && pages.front().get_size() == 0)
                pages.erase(pages.begin());

            append_vectors(pages, other.pages);
            num_rows += other.num_rows;
        }

        IntermediateResult(const IntermediateResult& o) = delete;
        IntermediateResult(IntermediateResult&& o) noexcept
            : pages(std::move(o.pages)), width(o.width), num_rows(o.num_rows) {
            o.width = 0;
            o.num_rows = 0;
            o.pages.clear();
        }

        IntermediateResult& operator=(const IntermediateResult&) = delete;
        IntermediateResult& operator=(IntermediateResult&& o) noexcept {
            if (this != &o) {
                pages = std::move(o.pages);
                width = o.width;
                num_rows = o.num_rows;
                o.width = 0;
                o.num_rows = 0;
                o.pages.clear();
            }
            return *this;
        }

    public:
        std::vector<DataPage> pages;
    private:
        size_t width, num_rows;
};