#pragma once

#include <task.h>
#include <join_result.h>
#include <data_column.h>
#include <inter_column.h>
#include <vector>
#include <unchained_table.h>
#include <partition.h>

class JoinDispatcher;

template <typename T>
class PartitionTask : public Task {
    public:
        PartitionTask(size_t first_page, size_t page_count, const JoinResult& build_side,
             std::shared_ptr<JoinDispatcher> join_dispatcher, int priority, size_t num_partitions, Partitions& mini_parts, DataType type)
            : Task(priority), first_page(first_page), page_count(page_count), num_partitions(num_partitions), mini_parts(mini_parts),  
            build_side(build_side), join_dispatcher(join_dispatcher){}

    protected:
        void run_task() override {
            assert(num_partitions);
            size_t build_size = build_side.get_output_size();

            if (build_side.is_from_scan()) {
                const DataColumn& b_column = build_side.result_col_scan;
                size_t last_record = std::min((first_page + (page_count)) * CAP_PER_PAGE, build_side.get_output_size());

                if(b_column.pagenated()) {
                    DataColumn::PageIterator it = b_column.page_iterator(first_page * CAP_PER_PAGE, last_record - 1);
                    for (; !it.end(); ++it) {
                        T key = it.retrieve_value<T>();
                        size_t hash = unchained_table<T>::hash(key);
                        size_t partition_id = hash >> (__builtin_clzll(num_partitions) + 1);
                        PartitionEntry<T> takis = { .entry = { .key = key, .row_id = it.get_index() }, .hash = hash };
                        mini_parts.insert<T>(partition_id, takis);
                    }
                } else {
                    super_id super_idx = { .row = 0 };
                    for (super_idx.row = first_page * CAP_PER_PAGE; super_idx.row < last_record; super_idx.row++) {
                        if (!b_column.is_valid(super_idx.row))
                            continue;
                        T key = b_column.retrieve_value<T>(super_idx.row);
                        size_t hash = unchained_table<T>::hash(key);
                        size_t partition_id = hash >> (__builtin_clzll(num_partitions) + 1);
                        PartitionEntry<T> takis = { .entry = { .key = key, .row_id = super_idx }, .hash = hash };
                        mini_parts.insert<T>(partition_id, takis);
                    }
                }
            }
            else {
                const IntermediateColumn& b_column = build_side.result_col_join;
                super_id super = { .offset = { .page = 0, .offset = 0 } };
                for(super.offset.page = first_page; (int32_t)super.offset.page < first_page+page_count; super.offset.page++){
                    uint32_t row_count = build_side.result_rowids.pages[super.offset.page].get_size();
                    for(super.offset.offset=row_count-1; (int32_t)super.offset.offset >= 0; super.offset.offset--) {
                        T key = b_column.retrieve_value<T>(super);
                        size_t hash = unchained_table<T>::hash(key);
                        size_t partition_id = hash >> (__builtin_clzll(num_partitions) + 1);
                        PartitionEntry<T> takis = { .entry = { .key = key, .row_id = super }, .hash = hash };
                        mini_parts.insert<T>(partition_id, takis);
                    }
                }
            }
        }

        void post_run_clarity() override;

    private:
        size_t first_page, page_count;
        size_t num_partitions;
        Partitions& mini_parts;
        const JoinResult& build_side;
        std::shared_ptr<JoinDispatcher> join_dispatcher;
};

extern template void PartitionTask<int32_t>::post_run_clarity();
extern template void PartitionTask<int64_t>::post_run_clarity();
extern template void PartitionTask<double>::post_run_clarity();
extern template void PartitionTask<std::string>::post_run_clarity();
