#pragma once

#include <vector>
#include <bitset>
#include <table.h>
#include <memory.h>
#include <cassert>
#include <custom_string.h>
#include <sync_primitives.h>
#include <defs.h>
#include <utils.h>

class DataColumn {
    public:
        DataColumn() : data(nullptr), bitmap(nullptr), num_rows(0) {};
        DataColumn(const Column& column, size_t num_rows, ColumnUse col_use, LoadDispatcherState& dispatcher_state);

        DataColumn(const DataColumn& o) = default;
        DataColumn(DataColumn&& o) noexcept = default;
        DataColumn& operator=(const DataColumn& o) = default;
        DataColumn& operator=(DataColumn&& o) noexcept = default;

        template <typename T>
        T retrieve_value(size_t row) const {
            assert(row < num_rows);

            if(is_pagenated){
                assert(elements_in_page > 0);
                size_t page = row / elements_in_page;
                size_t offset = row % elements_in_page;

                return *(reinterpret_cast<T*>(col_pages[page]) + offset);
            }

            T* temp = reinterpret_cast<T*>(data.get());
            return temp[row];
        }

        template <typename T>
        T retrieve_value_pageidx(size_t page_idx, size_t row) const {
            assert(is_pagenated);
            return *(reinterpret_cast<T*>(col_pages[page_idx]) + row);
        }

        size_t get_elements_in_page() const {
            assert(is_pagenated);
            return elements_in_page;
        }

        void* retrieve_value_ptr(size_t row) const {
            assert(row < num_rows);
            if(is_pagenated) {
                assert(elements_in_page > 0);
                assert(type_size > 0);
                size_t page = row / elements_in_page;
                size_t offset = row % elements_in_page;
                return reinterpret_cast<uint8_t*>(col_pages[page]) + (offset << type_shift);
            }

            return reinterpret_cast<void*>(reinterpret_cast<char*>(data.get()) + (row << type_shift));
        }

        bool is_valid(size_t row) const {
            assert(row < num_rows);
            if(is_pagenated) return true;
            return bitmap_check(bitmap.get(), row);
        }

        inline DataType get_type() const {
            return column_type;
        }

        inline size_t size() const {
            return num_rows;
        }

        inline bool pagenated() const {
            return is_pagenated;
        }

        inline size_t get_page_count() const {
            // Calculates ceil(num_rows / CAP_PER_PAGE)
            return (num_rows + CAP_PER_PAGE - 1) / CAP_PER_PAGE;
        }

        class PageIterator {
            public:
                PageIterator(const DataColumn* data_column, size_t start_row, size_t end_row, bool forwards) {
                    assert(data_column);
                    this->data_column = data_column;
                    this->elements_in_page = data_column->elements_in_page;
                    #ifndef NDEBUG
                    this->forwards = forwards;
                    #endif

                    assert(data_column->is_pagenated);
                    assert(start_row < data_column->size());
                    assert(end_row < data_column->size());
                    assert(elements_in_page > 0);

                    if(data_column->empty_page) {
                        if(forwards) {
                            page_idx = 1;
                            end_page = 0;
                        } else {
                            page_idx = 0;
                            end_page = 1;
                        }
                        offset = 0;
                        end_offset = 0;
                        index = 0;
                        return;
                    }

                    page_idx = start_row / elements_in_page;
                    offset = start_row % elements_in_page;

                    end_page = end_row / elements_in_page;
                    end_offset = end_row % elements_in_page;

                    index = start_row;

                    if(!forwards) {
                        std::swap(page_idx, end_page);
                        std::swap(offset, end_offset);
                        index = end_row;
                    }

                }

                PageIterator& operator++() {
                    assert(forwards);

                    if(++offset >= elements_in_page) {
                        offset = 0;
                        ++page_idx;
                    }

                    ++index;

                    return *this;
                }

                PageIterator& operator--() {
                    assert(!forwards);

                    if((int64_t)--offset < 0) {
                        offset = elements_in_page - 1;
                        --page_idx;
                    }

                    --index;

                    return *this;
                }

                template <typename T>
                T retrieve_value() const {
                    assert(page_idx < data_column->col_pages.size());
                    assert(offset < elements_in_page);
                    return *(reinterpret_cast<T*>(data_column->col_pages[page_idx]) + offset);
                }

                uint32_t get_index() const {
                    return index;
                }

                bool end() const {
                    assert(forwards);
                    return page_idx > end_page || (page_idx == end_page && offset > end_offset);
                }

                bool r_end() const {
                    assert(!forwards);
                    return (int64_t)page_idx < (int64_t)end_page || (page_idx == end_page && offset < end_offset);
                }

            private:
                const DataColumn* data_column;
                size_t page_idx, offset, end_page, end_offset;
                size_t elements_in_page;
                uint32_t index;

                #ifndef NDEBUG
                bool forwards;
                #endif
        };

        PageIterator page_iterator(size_t start_row, size_t end_row) const {
            return PageIterator(this, start_row, end_row, true);
        }

        PageIterator r_page_iterator(size_t start_row, size_t end_row) const {
            return PageIterator(this, start_row, end_row, false);
        }

    private:
        template <typename T>
        void setup_column(const Column& column, size_t num_rows, LoadDispatcherState& dispatcher_state, ColumnUse col_use);

        bool is_packed_not_null(const Column& column);

        std::shared_ptr<void> data;
        std::shared_ptr<uint8_t> bitmap;

        size_t num_rows;

        DataType column_type;
        size_t type_size = 0;
        int type_shift = 0;

        bool is_pagenated = false;
        bool empty_page = false;
        std::vector<void*> col_pages;
        uint16_t elements_in_page = 0;
};
