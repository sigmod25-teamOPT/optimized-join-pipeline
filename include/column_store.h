#pragma once

#include <vector>
#include <table.h>
#include <data_column.h>
#include <defs.h>
#include <cassert>

class ColumnStore {
    public:
        ColumnStore(const ColumnarTable& table, const std::vector<ColumnUse>& column_use, LoadDispatcherState& dispatcher_state) :
         num_rows(table.num_rows), num_cols(table.columns.size()) {
            assert(column_use.size() == num_cols);
            columns.reserve(num_cols);

            for (size_t i = 0; i < num_cols; i++)
                columns.emplace_back(table.columns[i], num_rows, column_use[i], dispatcher_state);

            assert(columns.size() == num_cols);
        }

        template <typename T>
        T& retrieve_value(size_t row, size_t column);

        inline DataColumn& get_column(size_t column) {
            assert(column < columns.size());
            return columns[column];
        }

    private:
        std::vector<DataColumn> columns;
        size_t num_rows, num_cols;
};

template <typename T>
T& ColumnStore::retrieve_value(size_t row, size_t column) {
    assert(column < columns.size());
    assert(row < num_rows);

    return columns[column].retrieve_value<T>(row);
}