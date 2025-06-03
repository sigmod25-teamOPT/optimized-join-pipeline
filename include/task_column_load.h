#pragma once

#include <task.h>
#include <sync_primitives.h>
#include <custom_string.h>
#include <utils.h>

class JoinDispatcher;

#define for_each_page(page, pages)      \
    for (std::byte* page: pages         \
        | ranges::views::transform([](auto* page) { return page->data; }))

class LoadTask : public Task {
    public:
        LoadTask(LoadDispatcherState& dispatcher_state, int priority) :
            Task(priority), root(nullptr), dispatcher_state(dispatcher_state) {}

       void set_root(std::shared_ptr<JoinDispatcher> root) {
            this->root = root;
       }

    protected:
        void post_run_clarity() override;

    private:
        std::shared_ptr<JoinDispatcher> root;
        LoadDispatcherState& dispatcher_state;
};

template <typename T>
class ColumnLoadTask : public LoadTask {
    public:
        ColumnLoadTask(size_t first_row, size_t num_rows, const std::vector<Page*>& pages, T* data, uint8_t* bitmap, LoadDispatcherState& dispatcher_state, int priority)
            : LoadTask(dispatcher_state, priority), first_row(first_row), num_rows(num_rows), pages(pages), data(data), bitmap(bitmap) {}

    protected:

        void run_task() override {
            size_t page_idx = 0;
            size_t rows_encountered = 0;
            size_t rows_added = 0;

            // skip pages
            for(page_idx = 0; page_idx<pages.size(); page_idx++){
                uint16_t num_rows = *reinterpret_cast<uint16_t*>(pages[page_idx]->data);
                if(rows_encountered + num_rows >= first_row)
                    break;

                rows_encountered += num_rows;
            }

            // skip rows of first page
            std::byte* first_page = pages[page_idx]->data;
            uint16_t num_rows_in_page = *reinterpret_cast<uint16_t*>(first_page);
            T* data_begin = reinterpret_cast<T*>(first_page + 4);
            uint8_t* page_bitmap = reinterpret_cast<uint8_t*>(first_page + PAGE_SIZE - (num_rows_in_page + 7) / 8);

            uint16_t data_idx = 0;
            uint16_t j = 0;
            size_t i = first_row;
            for (j = 0; j < first_row - rows_encountered; j++) {
                if (!bitmap_check(page_bitmap, j))
                    continue;

                data_idx++;
            }

            // fill in data of first page
            for (j = j; j < num_rows_in_page && rows_added < num_rows; j++, rows_added++) {
                if (!bitmap_check(page_bitmap, j)) {
                    i++;
                    continue;
                }

                auto value = data_begin[data_idx++];
                bitmap_set(bitmap, i);
                data[i++] = value;
            }

            // iterate all remaining pages
            for(page_idx = page_idx+1; page_idx<pages.size(); page_idx++){
                std::byte* page = pages[page_idx]->data;
                uint16_t num_rows_in_page = *reinterpret_cast<uint16_t*>(page);
                T* data_begin = reinterpret_cast<T*>(page + 4);

                if (num_rows_in_page == *reinterpret_cast<uint16_t*>(page + 2)) {
                    size_t to_copy = rows_added + num_rows_in_page > num_rows ? num_rows - rows_added : num_rows_in_page;
                    fastcpy(data + i, data_begin, to_copy * sizeof(T));
                    bitmap_setbatch(bitmap, i, to_copy);
                    rows_added += to_copy;
                    i += to_copy;
                    if (rows_added == num_rows) break;
                    continue;
                }

                if (*reinterpret_cast<uint16_t*>(page + 2) == 0) {
                    size_t to_copy = rows_added + num_rows_in_page > num_rows ? num_rows - rows_added : num_rows_in_page;
                    rows_added += to_copy;
                    i += to_copy;
                    if (rows_added == num_rows) break;
                    continue;
                }

                uint8_t* page_bitmap = reinterpret_cast<uint8_t*>(page + PAGE_SIZE - (num_rows_in_page + 7) / 8);
                data_idx = 0;

                for (j = 0; j < num_rows_in_page && rows_added < num_rows; j++, rows_added++) {
                    if (!bitmap_check(page_bitmap, j)) {
                        i++;
                        continue;
                    }

                    auto value = data_begin[data_idx++];
                    bitmap_set(bitmap, i);
                    data[i++] = value;
                }
                if(rows_added == num_rows)
                    break;
            }
        }

    private:
        size_t first_row, num_rows;
        const std::vector<Page*>& pages;
        T* data;
        uint8_t* bitmap;
};

template <>
class ColumnLoadTask<ColString> : public LoadTask {
    public:
        ColumnLoadTask(size_t first_row, size_t num_rows, const std::vector<Page*>& pages, ColString* data, uint8_t* bitmap,
                LoadDispatcherState& dispatcher_state, int priority)
            : LoadTask(dispatcher_state, priority), first_row(first_row), num_rows(num_rows), pages(pages), data(data), bitmap(bitmap) {}

    protected:

        void run_task() override {

            uint32_t page_idx = 0;
            size_t rows_encountered = 0;

            // Skip to desired page
            for (page_idx = 0; page_idx < pages.size(); page_idx++) {
                uint16_t num_rows_in_page = *reinterpret_cast<uint16_t*>(pages[page_idx]->data);

                // Handle special cases
                if (num_rows_in_page == 0xffff)
                    num_rows_in_page = 1;
                else if (num_rows_in_page == 0xfffe)
                    continue;

                if (rows_encountered + num_rows_in_page >= first_row)
                    break;

                rows_encountered += num_rows_in_page;
            }

            // Skip rows of first page
            uint32_t data_idx = 0;
            size_t i = first_row;
            size_t rows_added = 0;
            uint16_t j = 0;

            std::byte* first_page = pages[page_idx]->data;
            uint16_t num_rows_in_page = *reinterpret_cast<uint16_t*>(first_page);
            if (num_rows_in_page != 0xffff) { // Make sure there is something to skip
                uint8_t* page_bitmap =
                    reinterpret_cast<uint8_t*>(first_page + PAGE_SIZE - (num_rows_in_page + 7) / 8);
                for (j = 0; j < first_row - rows_encountered; j++) {
                    if (!bitmap_check(page_bitmap, j))
                        continue;
                    data_idx++;
                }
            }

            // Fill in data of first page
            for (; j < num_rows_in_page && rows_added < num_rows; ++j) {
                assert(num_rows_in_page != 0xfffe); // 0XFFFE IS NOT POSSIBLE
                if (num_rows_in_page == 0xffff) {
                    bitmap_set(bitmap, i);
                    data[i++] = ColString{page_idx, -1u};
                    rows_added++;
                    break;
                } else {
                    uint8_t* page_bitmap =
                        reinterpret_cast<uint8_t*>(first_page + PAGE_SIZE - (num_rows_in_page + 7) / 8);
                    if (!bitmap_check(page_bitmap, j)) {
                        i++;
                        continue;
                    }

                    bitmap_set(bitmap, i);
                    data[i++] = ColString{page_idx, data_idx++};
                    rows_added++;
                }
            }

            // Iterate all remaining pages
            for (page_idx = page_idx + 1; page_idx < pages.size(); page_idx++) {
                std::byte* page = pages[page_idx]->data;
                uint16_t num_rows_in_page = *reinterpret_cast<uint16_t*>(page);
                if (num_rows_in_page == 0xffff) {
                    bitmap_set(bitmap, i);
                    data[i++] = ColString{page_idx, -1u};
                    rows_added++;
                    continue;
                } else if (num_rows_in_page == 0xfffe) continue;

                if (num_rows_in_page == *reinterpret_cast<uint16_t*>(page + 2)) {
                    size_t to_copy = rows_added + num_rows_in_page > num_rows ? num_rows - rows_added : num_rows_in_page;
                    for (uint32_t k = 0; k < to_copy; k++) {
                        data[i + k] = ColString{page_idx, k};
                    }
                    bitmap_setbatch(bitmap, i, to_copy);
                    rows_added += to_copy;
                    i += to_copy;
                    if (rows_added == num_rows) break;
                    continue;
                }

                if (*reinterpret_cast<uint16_t*>(page + 2) == 0) {
                    size_t to_copy = rows_added + num_rows_in_page > num_rows ? num_rows - rows_added : num_rows_in_page;
                    rows_added += to_copy;
                    i += to_copy;
                    if (rows_added == num_rows) break;
                    continue;
                }

                uint8_t* page_bitmap =
                    reinterpret_cast<uint8_t*>(page + PAGE_SIZE - (num_rows_in_page + 7) / 8);
                data_idx = 0;

                for (j = 0; j < num_rows_in_page && rows_added < num_rows; j++) {
                    if (!bitmap_check(page_bitmap, j)) {
                        i++;
                        continue;
                    }

                    bitmap_set(bitmap, i);
                    data[i++] = ColString{page_idx, data_idx++};
                    rows_added++;
                }
                if (rows_added == num_rows)
                    break;
            }

        }

    private:
        size_t first_row, num_rows;
        const std::vector<Page*>& pages;
        ColString* data;
        uint8_t* bitmap;
};

class BatchLoadTask : public LoadTask {
    public:
        BatchLoadTask(std::vector<std::shared_ptr<Task>> batched_tasks, LoadDispatcherState& dispatcher_state, int priority)
            : LoadTask(dispatcher_state, priority), batched_tasks(std::move(batched_tasks)) {}

        void run_task() override {
            for (auto& task : batched_tasks)
                task->run_no_dispatch();
        }

    private:
        std::vector<std::shared_ptr<Task>> batched_tasks;
};
