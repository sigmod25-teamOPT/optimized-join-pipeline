#ifndef UTILS_H
#define UTILS_H

#include <cstdlib>
#include <stdlib.h>
#include <attribute.h>
#include <cstring>
#include <hardware.h>

#define concat_vectors(out, a, b)                   \
    do {                                            \
        out.reserve(a.size() + b.size());           \
        out.insert(out.end(), a.begin(), a.end());  \
        out.insert(out.end(), b.begin(), b.end());  \
    } while (0)

#define append_vectors(a, b)                        \
    do {                                            \
        a.reserve(a.size() + b.size());             \
        a.insert(a.end(), b.begin(), b.end());      \
    } while (0)

#define MEMCHECK(ptr) do { \
    if ((ptr) == nullptr) { \
        exit(123456); \
    } \
} while (0)

#define is_arithemtic_type(type)        \
   (type == DataType::INT32         ||  \
    type == DataType::INT64         ||  \
    type == DataType::FP64)

#define is_string_type(type)    \
    (type == DataType::VARCHAR)

static inline void *fastmemset(void *dest, int c, size_t len) {
    return memset(dest, c, len);
}

/* Bitmap helper functions */
static inline void bitmap_set(uint8_t* bitmap, size_t i) {
    bitmap[i / 8] |= (1 << (i % 8));
}

static void bitmap_setbatch(uint8_t *bitmap, size_t start, size_t length) {
    size_t byte_start = start / 8;
    size_t bit_offset = start % 8;
    size_t byte_end = (start + length) / 8;
    size_t bit_end_offset = (start + length) % 8;

    // Handle leading partial byte
    if (bit_offset != 0) {
        size_t bits_in_first_byte = std::min(length, 8 - bit_offset);
        uint8_t mask = ((1 << bits_in_first_byte) - 1) << bit_offset;
        bitmap[byte_start++] |= mask;
        length -= bits_in_first_byte;
    }

    // Use memset for fully aligned bytes
    if (length >= 8) {
        fastmemset(&bitmap[byte_start], 0xFF, length / 8);
        byte_start += length / 8;
        length %= 8;
    }

    // Handle trailing partial byte
    if (length > 0) {
        uint8_t mask = (1 << length) - 1;
        bitmap[byte_start] |= mask;
    }
}

static inline void bitmap_unset(uint8_t* bitmap, size_t i) {
    bitmap[i / 8] &= ~(1 << (i % 8));
}

static inline bool bitmap_check(uint8_t* bitmap, size_t i) {
    return  bitmap[i / 8] & (1 << (i % 8));
}

static inline int fastcmp(const void *s1, const void *s2, size_t len) {
    return memcmp(s1, s2, len);
}

static inline void fastcpy(void *dest, const void *src, size_t len) {
    memcpy(dest, src, len);
}

template <typename T>
static inline DataType reduce_template(){
    if constexpr (std::is_same_v<T, int32_t>)
        return DataType::INT32;
    else if constexpr (std::is_same_v<T, int64_t>)
        return DataType::INT64;
    else if constexpr (std::is_same_v<T, double>)
        return DataType::FP64;
    else{   //why reduce strings
        assert(false);
    }
    exit(1);
}

inline size_t get_type_size(DataType column_type) {
    switch (column_type) {
        case DataType::INT32:
            return sizeof(int32_t);
        case DataType::INT64:
            return sizeof(int64_t);
        case DataType::FP64:
            return sizeof(double);
        default:
            assert(false);
            return -1;
    }
}

#endif // UTILS_H