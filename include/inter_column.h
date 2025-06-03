#pragma once

#include <cassert>
#include <table.h>
#include <stddef.h>
#include <data_page.h>

class IntermediateColumn {
    public:
        IntermediateColumn() : num_rows(0){
            #ifndef NDEBUG
            do_not_use = true;
            #endif
        }

        IntermediateColumn(DataType type): num_rows(0) {
            pages.reserve(4096);
        }

        IntermediateColumn(IntermediateColumn&& o) noexcept
                : pages(std::move(o.pages)), num_rows(o.num_rows) {
            if (this == &o) return;
            o.pages.clear();
            o.num_rows = 0;
            #ifndef NDEBUG
            do_not_use = o.do_not_use;
            #endif
        }

        IntermediateColumn& operator=(IntermediateColumn&& o) {
            if (this == &o) return *this;
            pages = std::move(o.pages);
            num_rows = o.num_rows;
            o.pages.clear();
            o.num_rows = 0;
            #ifndef NDEBUG
            do_not_use = o.do_not_use;
            #endif
            return *this;
        }

        IntermediateColumn(const IntermediateColumn& o) = delete;
        IntermediateColumn& operator=(const IntermediateColumn&) = delete;

        ~IntermediateColumn() {
            pages.clear();
        }

        template <typename T>
        void insert(T value){
            assert(!do_not_use);
            if (pages.empty() || pages.back().full())
                pages.emplace_back(sizeof(T));

            pages.back().insert(value);
            num_rows++;
        }

        void concat(IntermediateColumn& other) {
            if(pages.size() && pages.front().get_size() == 0)
                pages.erase(pages.begin());

            append_vectors(pages, other.pages);
            num_rows += other.num_rows;
        }

        template <typename T>
        T retrieve_value(super_id row) const {
            assert(!do_not_use);
            size_t page_idx = row.offset.page;
            size_t row_idx = row.offset.offset;
            assert(page_idx < pages.size());
            assert(row_idx < pages[page_idx].get_size());
            return pages[page_idx].get<T>(row_idx);
        }

        size_t size() const {
            return num_rows;
        }

    public:
        std::vector<DataPage> pages;
    private:
        size_t num_rows;
        #ifndef NDEBUG
        bool do_not_use = false;
        #endif
};
