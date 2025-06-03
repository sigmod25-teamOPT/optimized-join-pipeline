#pragma once

#include <task.h>
#include <join_result.h>
#include <data_column.h>
#include <inter_column.h>
#include <vector>
#include <unchained_table.h>

class JoinDispatcher;

template <typename T>
class BuildingTask : public Task {
    public:
        BuildingTask(unchained_table<T>& hash_table, Partitions& partition_tuples,
                std::shared_ptr<JoinDispatcher> join_dispatcher, int priority, size_t partition_id,
                size_t num_partitions, size_t prevCount)
            : Task(priority), hash_table(hash_table), partition_tuples(partition_tuples),
                join_dispatcher(join_dispatcher), partition_id(partition_id), num_partitions(num_partitions),
                prevCount(prevCount) {}

    protected:
        void run_task() override {
            hash_table.insert(partition_tuples, partition_id, prevCount, num_partitions);
        }

        void post_run_clarity() override;

    private:
        unchained_table<T>& hash_table;
        Partitions& partition_tuples;
        std::shared_ptr<JoinDispatcher> join_dispatcher;
        size_t partition_id, num_partitions, prevCount;
};

extern template void BuildingTask<int32_t>::post_run_clarity();
extern template void BuildingTask<int64_t>::post_run_clarity();
extern template void BuildingTask<double>::post_run_clarity();
extern template void BuildingTask<std::string>::post_run_clarity();