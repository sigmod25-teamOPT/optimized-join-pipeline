#pragma once
#include <cassert>
#include <utils.h>
#include <stddef.h>
#include <iostream>

typedef union super_id {
    struct {
        uint32_t page;
        uint32_t offset;
    } offset;
    uint64_t row;
} super_id;

class DataPage {
    public:
        DataPage(size_t type_size) : size(0), type_size(type_size) {
            assert(type_size);
            data = std::shared_ptr<uint8_t>(new uint8_t[type_size * CAP_PER_PAGE](), std::default_delete<uint8_t[]>());
            start_data_ptr = data.get();
            data_ptr = start_data_ptr;
        }

        template <typename T>
        void insert(T value) {
            assert(!full());
            *((T*)data_ptr)= value;

            size++;
            data_ptr += type_size;
        }

        void insert_parts(const void* left, size_t left_size, const void* right, size_t right_size) {
            assert(type_size % (left_size + right_size) == 0);
            assert(!full());

            fastcpy(data_ptr, left, left_size * sizeof(rid_t));
            fastcpy(data_ptr + left_size * sizeof(rid_t), right, right_size * sizeof(rid_t));

            size++;
            data_ptr += type_size;
        }

        template <typename T>
        T get(size_t idx) const {
            assert(idx < size);
            return *((T*)(start_data_ptr + idx * type_size));
        }

        void* get_ptr(size_t idx) const {
            assert(idx < size);
            return start_data_ptr + idx * type_size;
        }

        template <typename T>
        T operator[](size_t idx) const {
            return get<T>(idx);
        }

        bool full() const {
            return size == CAP_PER_PAGE;
        }

        size_t get_size() const {
            return size;
        }


    private:
        std::shared_ptr<uint8_t> data;
        uint8_t* start_data_ptr = nullptr;
        uint8_t* data_ptr = nullptr;
        size_t size; // number of elements in the page
        size_t type_size; // in bytes
};