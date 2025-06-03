#pragma once

#include <plan.h>
#include <stddef.h>
#include <vector>
#include <hardware_parse.h>

enum ColumnUse {
    UNUSED = 0,
    JOIN,
    RESULT_ONLY,
    COLUMN_USE_MAX
};

using Attributes = std::vector<std::tuple<size_t, DataType>>;

struct ColumnElement {
public:
    ColumnElement(uint8_t table_id, uint8_t column_id, uint8_t scan_id)
        : table_id(table_id), column_id(column_id), scan_id(scan_id) {}

    uint8_t table_id;
    uint8_t column_id;
    uint8_t scan_id;
};

#define NULL_DATA_COLUMN (*((DataColumn*)nullptr))

#define ROWS_PER_JOB 131072
#define THRESHOLD 0.4
#define MIN_ROWS_PER_JOB (THRESHOLD * ROWS_PER_JOB)
#define USE_BLOOMFILTER

#ifdef SPC__PPC64LE
    #define CAP_PER_PAGE (4 * 1024)  
#else
    #define CAP_PER_PAGE (2 * 1024)
#endif

typedef size_t rid_t;
