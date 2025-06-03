#pragma once
#include <cstddef>
#include <cstring>
#include <vector>
#include <data_column.h>
#include <custom_string.h>
#include <debug.h>
#include <utils.h>

static void columnar_append(ColumnarTable& dest, ColumnarTable& src) {
    if (src.num_rows == 0) return;
    assert(dest.columns.size() == src.columns.size());
    dest.num_rows += src.num_rows;
    for (size_t i = 0; i < dest.columns.size(); i++) {
        append_vectors(dest.columns[i].pages, src.columns[i].pages);
        src.columns[i].pages.clear();
    }

    src.columns.clear();
    src.num_rows = 0;
}

template <class T>
struct ColumnWrapperReal {
    const Column& origin;
    Column& column;
    size_t last_page_idx = 0;
    uint16_t num_rows = 0;
    size_t data_end = data_begin();
    std::vector<uint8_t> bitmap;

    constexpr static size_t data_begin() {
        if (sizeof(T) < 4) {
            return 4;
        } else {
            return sizeof(T);
        }
    }

    ColumnWrapperReal(Column& column, const Column& origin)
    : origin(origin), column(column) {
        bitmap.resize(PAGE_SIZE, -1);
    }

    std::byte* get_page() {
        if (last_page_idx == column.pages.size()) [[unlikely]] {
            column.new_page();
        }
        auto* page = column.pages[last_page_idx];
        return page->data;
    }

    void save_page() {
        auto* page = get_page();
        *reinterpret_cast<uint16_t*>(page) = num_rows;
        *reinterpret_cast<uint16_t*>(page + 2) =
            static_cast<uint16_t>((data_end - data_begin()) / sizeof(T));
        size_t bitmap_size = (num_rows + 7) / 8;
        fastcpy(page + PAGE_SIZE - bitmap_size, bitmap.data(), bitmap_size);
        ++last_page_idx;
        num_rows = 0;
        data_end = data_begin();
        std::fill(bitmap.begin(), bitmap.end(), -1);
    }

    void set_bitmap(size_t idx) {
        size_t byte_idx   = idx / 8;
        size_t bit_idx    = idx % 8;
        bitmap[byte_idx] |= (0x1 << bit_idx);
    }

    void unset_bitmap(size_t idx) {
        size_t byte_idx   = idx / 8;
        size_t bit_idx    = idx % 8;
        bitmap[byte_idx] &= ~(0x1 << bit_idx);
    }

    void insert(T value) {
        if (data_end + 4 + num_rows / 8 + 1 > PAGE_SIZE) [[unlikely]] {
            save_page();
        }
        auto* page                              = get_page();
        *reinterpret_cast<T*>(page + data_end)  = value;
        data_end                               += sizeof(T);
        // set_bitmap(num_rows);
        ++num_rows;
    }

    void insert_null() {
        if (data_end + num_rows / 8 + 1 > PAGE_SIZE) [[unlikely]] {
            save_page();
        }
        unset_bitmap(num_rows);
        ++num_rows;
    }

    void finalize() {
        if (num_rows != 0) {
            save_page();
        }
    }
};

template <>
struct ColumnWrapperReal<ColString> {
    const Column&        origin;
    Column&              column;
    size_t               last_page_idx = 0;
    uint16_t             num_rows      = 0;
    uint16_t             data_size     = 0;
    size_t               offset_end    = 4;
    std::vector<char>    data;
    std::vector<uint8_t> bitmap;

    constexpr static size_t offset_begin() { return 4; }

    ColumnWrapperReal(Column& column, const Column& origin)
    : origin(origin), column(column) {
        data.resize(PAGE_SIZE);
        bitmap.resize(PAGE_SIZE, -1);
    }

    std::byte* get_page() {
        if (last_page_idx == column.pages.size()) [[unlikely]] {
            column.new_page();
        }
        auto* page = column.pages[last_page_idx];
        return page->data;
    }

    void save_long_string(ColString value) {
        uint16_t pageid = value.pageid;
        auto* page = origin.pages[pageid++]->data;
        auto* dest = get_page();
        while (*reinterpret_cast<uint16_t*>(page) == 0xffff
            || *reinterpret_cast<uint16_t*>(page) == 0xfffe) {
            fastcpy(dest, page, PAGE_SIZE);
            ++last_page_idx;
            dest = get_page();
            page = origin.pages[pageid++]->data;
        }
    }

    void save_page() {
        auto* page                         = get_page();
        *reinterpret_cast<uint16_t*>(page) = num_rows;
        *reinterpret_cast<uint16_t*>(page + 2) =
            static_cast<uint16_t>((offset_end - offset_begin()) / 2);
        size_t bitmap_size = (num_rows + 7) / 8;
        fastcpy(page + offset_end, data.data(), data_size);
        fastcpy(page + PAGE_SIZE - bitmap_size, bitmap.data(), bitmap_size);
        ++last_page_idx;
        num_rows   = 0;
        data_size  = 0;
        offset_end = offset_begin();
        std::fill(bitmap.begin(), bitmap.end(), -1);
    }

    void set_bitmap(size_t idx) {
        size_t byte_idx   = idx / 8;
        size_t bit_idx    = idx % 8;
        bitmap[byte_idx] |= (0x1 << bit_idx);
    }

    void unset_bitmap(size_t idx) {
        size_t byte_idx   = idx / 8;
        size_t bit_idx    = idx % 8;
        bitmap[byte_idx] &= ~(0x1 << bit_idx);
    }

    void insert(ColString value) {
        if (value.is_long_string()) {
            if (num_rows > 0) {
                save_page();
            }
            save_long_string(value);
        } else {
            auto* page = origin.pages[value.pageid]->data;
            auto page_num_rows = *reinterpret_cast<uint16_t*>(page);
            uint16_t* offset = reinterpret_cast<uint16_t*>(page + 4);
            uint16_t this_offset = offset[value.offset];
            auto* data_begin = page + 4 + page_num_rows * 2;
            auto* value_end = page + 4 + page_num_rows * 2 + this_offset;

            auto* value_start = value.offset == 0 ? data_begin : data_begin + offset[value.offset - 1];
            auto len = value_end - value_start;
            if (offset_end + sizeof(uint16_t) + data_size + len + num_rows / 8 + 1 > PAGE_SIZE) {
                save_page();
            }
            fastcpy(data.data() + data_size, value_start, len);
            data_size += len;
            auto* dest = get_page();
            *reinterpret_cast<uint16_t*>(dest + offset_end) = data_size;
            offset_end += sizeof(uint16_t);
            // set_bitmap(num_rows);
            ++num_rows;
        }
    }

    void insert_null() {
        if (offset_end + data_size + num_rows / 8 + 1 > PAGE_SIZE) [[unlikely]] {
            save_page();
        }
        unset_bitmap(num_rows);
        ++num_rows;
    }

    void finalize() {
        if (num_rows != 0) {
            save_page();
        }
    }
};




#include <iostream>

class ColumnWrapper {
private:
    DataType type;
    void *real;

public:

    ColumnWrapper(DataType type, Column& column, const Column& origin) : type(type) {
        switch (type) {
            case DataType::INT32:
                real = new ColumnWrapperReal<int32_t>(column, origin);
                break;
            case DataType::INT64:
                real = new ColumnWrapperReal<int64_t>(column, origin);
                break;
            case DataType::FP64:
                real = new ColumnWrapperReal<double>(column, origin);
                break;
            case DataType::VARCHAR:
                real = new ColumnWrapperReal<ColString>(column, origin);
                break;
            default:
                assert(false && "Unsupported data type");
                break;
        }
    }

    void insert(void *value) {
        switch (type) {
            case DataType::INT32:
                (reinterpret_cast<ColumnWrapperReal<int32_t>*>(real))->insert(*reinterpret_cast<int32_t*>(value));
                break;
            case DataType::INT64:
                (reinterpret_cast<ColumnWrapperReal<int64_t>*>(real))->insert(*reinterpret_cast<int64_t*>(value));
                break;
            case DataType::FP64:
                (reinterpret_cast<ColumnWrapperReal<double>*>(real))->insert(*reinterpret_cast<double*>(value));
                break;
            case DataType::VARCHAR:
                (reinterpret_cast<ColumnWrapperReal<ColString>*>(real))->insert(*reinterpret_cast<ColString*>(value));
                break;
            default:
                assert(false && "Unsupported data type");
                break;
        }
    }

    void insert_null() {
        switch (type) {
            case DataType::INT32:
                (reinterpret_cast<ColumnWrapperReal<int32_t>*>(real))->insert_null();
                break;
            case DataType::INT64:
                (reinterpret_cast<ColumnWrapperReal<int64_t>*>(real))->insert_null();
                break;
            case DataType::FP64:
                (reinterpret_cast<ColumnWrapperReal<double>*>(real))->insert_null();
                break;
            case DataType::VARCHAR:
                (reinterpret_cast<ColumnWrapperReal<ColString>*>(real))->insert_null();
                break;
            default:
                assert(false && "Unsupported data type");
                break;
        }
    }

    void finalize() {
        switch (type) {
            case DataType::INT32:
                (reinterpret_cast<ColumnWrapperReal<int32_t>*>(real))->finalize();
                break;
            case DataType::INT64:
                (reinterpret_cast<ColumnWrapperReal<int64_t>*>(real))->finalize();
                break;
            case DataType::FP64:
                (reinterpret_cast<ColumnWrapperReal<double>*>(real))->finalize();
                break;
            case DataType::VARCHAR:
                (reinterpret_cast<ColumnWrapperReal<ColString>*>(real))->finalize();
                break;
            default:
                assert(false && "Unsupported data type");
                break;
        }
    }

    inline DataType get_type() const {
        return type;
    }

    ~ColumnWrapper() {
        switch (type) {
            case DataType::INT32:
                delete reinterpret_cast<ColumnWrapperReal<int32_t>*>(real);
                break;
            case DataType::INT64:
                delete reinterpret_cast<ColumnWrapperReal<int64_t>*>(real);
                break;
            case DataType::FP64:
                delete reinterpret_cast<ColumnWrapperReal<double>*>(real);
                break;
            case DataType::VARCHAR:
                delete reinterpret_cast<ColumnWrapperReal<ColString>*>(real);
                break;
            default:
                assert(false && "Unsupported data type");
                break;
        }
    }
};