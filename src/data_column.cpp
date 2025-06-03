#include <debug.h>
#include <data_column.h>
#include <task_queue.h>
#include <task_column_load.h>

DataColumn::DataColumn(const Column& column, size_t num_rows, ColumnUse col_use, LoadDispatcherState& dispatcher_state)
    : data(nullptr), bitmap(nullptr), num_rows(num_rows), column_type(column.type)
{
    assert(col_use >= 0 && col_use < ColumnUse::COLUMN_USE_MAX);

    if(col_use == ColumnUse::UNUSED || num_rows == 0 || column.pages.size() == 0)
        return;

    assert(col_use != ColumnUse::UNUSED);

    if(is_arithemtic_type(column.type) && is_packed_not_null(column)) {
        is_pagenated = true;

        assert(column.pages.size() > 0);
        elements_in_page = *(reinterpret_cast<uint16_t*>(column.pages[0]->data));
        if(elements_in_page == 0) {
            elements_in_page = 1;
            empty_page = true;
        }

        type_size = get_type_size(column.type);
        type_shift = __builtin_ctzll(type_size);

        return;
    }

    size_t bitmap_size_in_bytes = (num_rows + 7) / 8;
    bitmap = std::shared_ptr<uint8_t>(new uint8_t[bitmap_size_in_bytes](), std::default_delete<uint8_t[]>());

    switch (column_type) {
        case DataType::INT32:
            data = std::shared_ptr<void>(new int32_t[num_rows], std::default_delete<int32_t[]>());
            type_size = sizeof(int32_t);
            type_shift = __builtin_ctzll(type_size);
            setup_column<int32_t>(column, num_rows, dispatcher_state, col_use);
            break;
        case DataType::INT64:
            data = std::shared_ptr<void>(new int64_t[num_rows], std::default_delete<int64_t[]>());
            type_size = sizeof(int64_t);
            type_shift = __builtin_ctzll(type_size);
            setup_column<int64_t>(column, num_rows, dispatcher_state, col_use);
            break;
        case DataType::FP64:
            data = std::shared_ptr<void>(new double[num_rows], std::default_delete<double[]>());
            type_size = sizeof(double);
            type_shift = __builtin_ctzll(type_size);
            setup_column<double>(column, num_rows, dispatcher_state, col_use);
            break;
        case DataType::VARCHAR:
            if(col_use != ColumnUse::JOIN) {
                data = std::shared_ptr<void>(new ColString[num_rows], std::default_delete<ColString[]>());
                type_size = sizeof(ColString);
                type_shift = __builtin_ctzll(type_size);
                setup_column<ColString>(column, num_rows, dispatcher_state, col_use);
            } else
                assert(false);

            break;
        default:
            assert(false);
    }

    assert(data);
}

template <typename T>
void DataColumn::setup_column(const Column& column, size_t num_rows, LoadDispatcherState& dispatcher_state, ColumnUse col_use) {
    assert(col_use != UNUSED);
    T* data = reinterpret_cast<T*>(this->data.get());

    size_t num_tasks = num_rows / ROWS_PER_JOB;
    num_tasks += ((num_rows % ROWS_PER_JOB) ? 1 : 0);

    for (size_t i = 0; i < num_tasks; i++) {
        size_t rows_in_task = (num_rows - i * ROWS_PER_JOB) >= ROWS_PER_JOB ? ROWS_PER_JOB : (num_rows - i * ROWS_PER_JOB);

        auto task = std::make_shared<ColumnLoadTask<T>>(
            i * ROWS_PER_JOB, rows_in_task, column.pages, data, bitmap.get(), dispatcher_state, 0
        );

        dispatcher_state.register_load_task(std::move(task), rows_in_task, col_use);
    }
}

bool DataColumn::is_packed_not_null(const Column& column) {
    col_pages.resize(column.pages.size());

    int rows = -1;
    int data_begin;
    switch (column.type) {
        case DataType::INT32:
            data_begin = 4;
            break;
        case DataType::INT64:
            data_begin = 8;
            break;
        case DataType::FP64:
            data_begin = 8;
            break;
        default:
            assert(false);
    }

    for(int i=0; i < column.pages.size(); i++){
        uint16_t all = *reinterpret_cast<uint16_t*>(column.pages[i]->data);
        uint16_t not_null = *reinterpret_cast<uint16_t*>(column.pages[i]->data + 2);
        if(all != not_null)
            return false;

        col_pages[i] = reinterpret_cast<uint8_t*>(column.pages[i]->data) + data_begin;

        if(i==0) {
            rows = all;
            continue;
        }

        if(all != rows && i != column.pages.size()-1)
            return false;
    }

    return true;
}
