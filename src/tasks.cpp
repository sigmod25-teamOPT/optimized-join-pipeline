#include <probing_task.h>
#include <building_task.h>
#include <task_column_load.h>
#include <join_state.h>
#include <attribute.h>

template <typename T, typename N>
void ProbingTask<T, N>::post_run_clarity() {
    join_dispatcher->probe_task_done(std::move(results));
}

template void ProbingTask<int32_t, int32_t>::post_run_clarity();
template void ProbingTask<int32_t, int64_t>::post_run_clarity();
template void ProbingTask<int64_t, int32_t>::post_run_clarity();
template void ProbingTask<int64_t, int64_t>::post_run_clarity();

template <typename T, typename N>
void ProbingTaskRoot<T, N>::post_run_clarity() {
    join_dispatcher->probe_task_done(columnar_table);
}

template void ProbingTaskRoot<int32_t, int32_t>::post_run_clarity();
template void ProbingTaskRoot<int32_t, int64_t>::post_run_clarity();
template void ProbingTaskRoot<int64_t, int32_t>::post_run_clarity();
template void ProbingTaskRoot<int64_t, int64_t>::post_run_clarity();

void DummyProbingTask::post_run_clarity() {
    join_dispatcher->probe_task_done(JoinResult(69));
}

void DummyProbingTaskRoot::post_run_clarity(){
    ColumnarTable table;
    join_dispatcher->probe_task_done(table);
}

template <typename T>
void BuildingTask<T>::post_run_clarity() {
    join_dispatcher->build_task_done();
}

template void BuildingTask<int32_t>::post_run_clarity();
template void BuildingTask<int64_t>::post_run_clarity();

template <typename T>
void PartitionTask<T>::post_run_clarity() {
    join_dispatcher->partition_task_done();
}

template void PartitionTask<int32_t>::post_run_clarity();
template void PartitionTask<int64_t>::post_run_clarity();


void LoadTask::post_run_clarity() {
    if(root)
        root->col_load_task_done();
    else
        dispatcher_state.signal_task_done();
}
