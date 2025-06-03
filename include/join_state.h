#pragma once

#include <join_result.h>
#include <sync_primitives.h>
#include <execution_instance.h>
#include <probing_task.h>
#include <partition_task.h>
#include <building_task.h>
#include <task_queue.h>
#include <plan.h>
#include <utils.h>
#include <defs.h>
#include <memory>
#include <atomic>
#include <tuple>
#include <vector>
#include <thread>
#include <shared_mutex>
#include <unchained_table.h>
#include <partition.h>
#include <hardware_parse.h>
#include <pipeline_helpers.h>
#include <pages_feeder.h>
typedef std::mutex DispatcherLock;

constexpr size_t MINIMUM_PARTITION_SIZE = 0x8000 / sizeof(PartitionEntry<int64_t>);

// Will return more than 2 parts
static size_t decide_parts(size_t num_rows) {
    constexpr size_t MAXIMUM_PARTITIONS = 1ull << (31 - __builtin_clz(P_SPC__CORE_COUNT));
    size_t parts = 1;
    while (num_rows / parts >= MINIMUM_PARTITION_SIZE) {
        parts <<= 1;
    }
    parts = std::max(std::min(parts, MAXIMUM_PARTITIONS), (size_t) 2);
    assert(__builtin_popcount(parts) == 1);
    return parts;
}


class JoinDispatcher : public std::enable_shared_from_this<JoinDispatcher> {
    public:
        JoinDispatcher(std::shared_ptr<JoinDispatcher> parent, bool data_for_probe, ExecutionInstance& instance,
            size_t nextjoincol, int priority)
            : build_result(42), parent(std::move(parent)), data_for_probe(data_for_probe),
                instance(instance), nextjoincol(nextjoincol), task_priority(priority)
        {
            build_queue_slots.reserve(TaskQueue::get_instance()->get_num_workers());
            probe_queue_slots.reserve(TaskQueue::get_instance()->get_num_workers());

            for (size_t i = 0; i < TaskQueue::get_instance()->get_num_workers(); ++i) {
                build_queue_slots.emplace_back(42);
                probe_queue_slots.emplace_back(69);
            }
        }

        void set_join_metadata(JoinMetadata metadata) {
            join_metadata = std::move(metadata);
            pre_probe_hook();
        }

        virtual ~JoinDispatcher() {}

        virtual void probe_task_done(JoinResult task_result) {};
        virtual void probe_task_done(ColumnarTable& columnar_table) {};
        virtual void build_task_done() = 0;
        virtual void col_load_task_done() {}
        virtual void partition_task_done() {}
        virtual void input_data_ready(JoinResult result, bool probe, bool final) = 0;

        virtual void wait() {}

    protected:
        virtual void spawn_partition_tasks() = 0;
        virtual void spawn_build_tasks() = 0;
        virtual void spawn_probe_tasks(JoinResult probe_data) = 0;

        virtual void pre_probe_hook() {}
        virtual void post_probe_hook() {}

        virtual JoinResult construct_join_result() = 0;

    protected:
        JoinMetadata join_metadata;
        std::atomic<PipelineState> pipeline_state{PipelineState::DEFAULT};

        JoinResult build_result;

        std::vector<JoinResult> build_queue_slots;
        std::vector<JoinResult> probe_queue_slots;
        std::vector<Partitions> partition_slots;

        // Protects insertion and access to the build queue
        std::shared_mutex probe_queue_lock;

        std::shared_ptr<JoinDispatcher> parent;
        bool data_for_probe;

        // Join state
        ExecutionInstance& instance;
        size_t nextjoincol;

        int task_priority;

        std::atomic<uint32_t> probe_tasks_registered{0};
        std::atomic<uint32_t> probe_tasks_finished{0};

        uint32_t part_tasks_registered{0};
        std::atomic<uint32_t> part_tasks_finished{0};

        uint32_t build_tasks_registered{0};
        std::atomic<uint32_t> build_tasks_finished{0};

        uint32_t col_load_tasks_registered{0};
        std::atomic<uint32_t> col_load_tasks_finished{0};

        std::atomic<bool> got_all_probe_data{false};
        std::atomic<bool> spawned_all_probe_tasks{false};
};

template <typename T, typename N>
class JoinDispatcherState : public JoinDispatcher {
    public:

        JoinDispatcherState(std::shared_ptr<JoinDispatcher> parent, bool data_for_probe, ExecutionInstance& instance,
             size_t nextjoincol, int priority)
             : JoinDispatcher(std::move(parent), data_for_probe, instance, nextjoincol, priority),
               partition_result(reduce_template<T>()), next_join_col_type(reduce_template<N>())
        {}

        virtual ~JoinDispatcherState() {}

        void partition_task_done() override {
            assert(part_tasks_registered > 0);

            if(atomic_inc_and_cmp(part_tasks_finished, part_tasks_registered))
                advance_to_building();
        }

        void build_task_done() override {
            // Build data ready
            assert(build_tasks_registered > 0);

            if(atomic_inc_and_cmp(build_tasks_finished, build_tasks_registered))
                advance_to_wait_columns();
        }

        void probe_task_done(JoinResult task_result) override {
            assert(probe_tasks_registered > 0);

            if(!task_result.is_empty())
                parent->input_data_ready(std::move(task_result), data_for_probe, false);

            if(atomic_inc_and_cmp(probe_tasks_finished, probe_tasks_registered)
                    && spawned_all_probe_tasks.load(std::memory_order_acquire)) {
                advance_to_finished();
                post_probe_hook();
                parent->input_data_ready(construct_join_result(), data_for_probe, true);
            }
        }

        void col_load_task_done() override {
            assert(false);
        }

        // Only one caller for probe or build can be active while final == true
        void input_data_ready(JoinResult result, bool probe, bool final) override {
            assert(!result.is_temporary());
            assert(!result.is_from_scan() || final);

            if (probe) {

                assert(pipeline_state.load(std::memory_order_acquire) >= DEFAULT);
                assert(pipeline_state.load(std::memory_order_acquire) <= JOINING);

                if(pipeline_state.load(std::memory_order_acquire) < PipelineState::JOINING) {
                    std::shared_lock<std::shared_mutex> lock(probe_queue_lock);

                    if(final)
                        got_all_probe_data = true;

                    if(pipeline_state.load(std::memory_order_acquire) < JOINING)
                        JoinResult::append_results(probe_queue_slots[std::max(worker_id, 0)], std::move(result));
                    else if(pipeline_state.load(std::memory_order_acquire) == JOINING) {
                        lock.unlock();
                        spawn_probe_tasks(std::move(result));
                    } else
                        assert(false);
                }
                else if (pipeline_state.load(std::memory_order_acquire) == JOINING){
                    if(final)
                        got_all_probe_data = true;
                    spawn_probe_tasks(std::move(result));
                }
                else
                    assert(false);
            }
            else {  // Build side
                assert(pipeline_state.load(std::memory_order_acquire) == PipelineState::DEFAULT);
                if(!final)
                    JoinResult::append_results(build_queue_slots[std::max(worker_id, 0)], std::move(result));
                else
                    advance_to_partitioning(std::move(result));
            }
        }

    protected:
        void spawn_partition_tasks() override {
            build_tasks_registered = decide_parts(build_result.get_output_size());

            size_t total_page_count = build_result.is_from_scan() ? (std::ceil(build_result.get_output_size() / (double)CAP_PER_PAGE)) :
                build_result.result_col_join.pages.size();

            assert(total_page_count > 0);

            /* Decide tasks to make */
            size_t tasks = total_page_count / build_tasks_registered;
            if(total_page_count % build_tasks_registered)
                tasks += 1;

            // Do not create more tasks than workers
            tasks = std::min(tasks, TaskQueue::get_instance()->get_num_workers());

            assert(tasks > 0);
            assert(tasks <= TaskQueue::get_instance()->get_num_workers());

            part_tasks_registered = tasks;

            /* Assign work to tasks */
            size_t pages_per_task = 0;
            size_t pages_remainder = 0;
            if(tasks > 0) {
                pages_per_task = total_page_count / tasks;
                pages_remainder = total_page_count % tasks;
            }
            size_t pages_assigned = 0;

            partition_slots.reserve(tasks);

            for(size_t i = 0; i < tasks; i++) {
                partition_slots.emplace_back(reduce_template<T>(), build_tasks_registered);
                size_t task_page_count = pages_per_task + (i < pages_remainder);

                auto task = std::make_shared<PartitionTask<T>>(pages_assigned, task_page_count, build_result,
                    shared_from_this(), task_priority, build_tasks_registered, partition_slots[i], reduce_template<T>());

                pages_assigned += task_page_count;

                if(i == tasks - 1)
                    TaskQueue::get_instance()->addTaskDirect(task);
                else
                    TaskQueue::get_instance()->addTask(task);
            }
        }

        void spawn_build_tasks() override {
            assert(build_tasks_registered > 0);
            assert(partition_result.size() == 0);

            partition_result.resize(build_tasks_registered);
            for (Partitions& part : partition_slots)
                partition_result.concat(part);

            assert(partition_result.size() > 1);
            hash_table.reserve(partition_result.size());

            size_t prev_count = 0;

            for(int i = 0; i < build_tasks_registered; i++) {
                auto task = std::make_shared<BuildingTask<T>>(
                    hash_table, partition_result, shared_from_this(), task_priority, i, build_tasks_registered, prev_count
                );

                prev_count += partition_result.counts[i];

                if(i == build_tasks_registered - 1)
                    TaskQueue::get_instance()->addTaskDirect(task);
                else
                    TaskQueue::get_instance()->addTask(task);
            }
        }

        template <template <typename, typename> class K, class Dummy>
        void spawn_probe_tasks_common(JoinResult probe_data) {
            assert(!spawned_all_probe_tasks.load(std::memory_order_acquire));

            if(probe_data.is_empty() || build_result.is_empty()) {
                probe_tasks_registered++;
                if(got_all_probe_data.load(std::memory_order_acquire))
                    spawned_all_probe_tasks = true;

                auto dummy_task = std::make_shared<Dummy>(shared_from_this());
                TaskQueue::get_instance()->addTaskDirect(dummy_task);
                return;
            }

            size_t total_page_count = probe_data.is_from_scan() ? (std::ceil(probe_data.get_output_size() / (double)CAP_PER_PAGE)) :
            probe_data.result_col_join.pages.size();

            assert(total_page_count > 0);

            size_t tasks = std::min(std::min(TaskQueue::get_instance()->get_num_workers(), total_page_count), (size_t)64);

            assert(tasks > 0);

            std::shared_ptr<JoinResult> probe_side = std::make_shared<JoinResult>(std::move(probe_data));

            probe_tasks_registered += tasks;

            if(got_all_probe_data.load(std::memory_order_acquire))
                spawned_all_probe_tasks = true;

            size_t pages_per_task = 0;
            size_t pages_remainder = 0;
            if(tasks > 0) {
                pages_per_task = total_page_count / tasks;
                pages_remainder = total_page_count % tasks;
            }
            size_t pages_assigned = 0;
            probe_side->pages_feeders = new PagesFeeder[tasks];

            /* Make them first cause we will have race conditions */
            for (size_t i = 0; i < tasks; i++) {
                size_t count = pages_per_task + (i < pages_remainder);
                probe_side->pages_feeders[i].set_param(pages_assigned, count);
                pages_assigned += count;
            }
            assert(pages_assigned == total_page_count);

            pages_assigned = 0;
            for (size_t i = 0; i < tasks; i++) {
                size_t task_page_count = pages_per_task + (i < pages_remainder);

                auto task = std::make_shared<K<T, N>>(
                    hash_table, shared_from_this(), instance, join_metadata, build_result,
                    probe_side, nextjoincol, task_priority, i, tasks
                );

                pages_assigned += task_page_count;

                if(i == tasks - 1)
                    TaskQueue::get_instance()->addTaskDirect(task);
                else
                    TaskQueue::get_instance()->addTask(task);
            }
        }

        void spawn_probe_tasks(JoinResult probe_data) override {
            this->spawn_probe_tasks_common<ProbingTask, DummyProbingTask>(std::move(probe_data));
        }

        void advance_to_partitioning(JoinResult result) {
            assert(pipeline_state.load(std::memory_order_acquire) == PipelineState::DEFAULT);

            if(try_advance_state(pipeline_state, PipelineState::PARTITIONING)) {
                for(JoinResult& build_slot : build_queue_slots)
                    JoinResult::append_results(build_result, std::move(build_slot));

                JoinResult::append_results(build_result, std::move(result));

                if(build_result.is_empty() || build_result.get_output_size() == 1) {
                    advance_to_wait_columns();
                    return;
                }

                spawn_partition_tasks();
            } else
                assert(false);
        }

        void advance_to_building() {
            assert(pipeline_state.load(std::memory_order_acquire) == PipelineState::PARTITIONING);

            if(try_advance_state(pipeline_state, PipelineState::BUILDING))
                spawn_build_tasks();
            else
                assert(false);
        }

        void advance_to_wait_columns() {
            assert(pipeline_state.load(std::memory_order_acquire) == PipelineState::PARTITIONING
                || pipeline_state.load(std::memory_order_acquire) == PipelineState::BUILDING);

            if(try_advance_state(pipeline_state, PipelineState::WAIT_COLUMNS)) {
                if(col_load_tasks_finished.load(std::memory_order_acquire) == col_load_tasks_registered)
                    advance_to_joining();
            }
            else
                assert(false);
        }

        void advance_to_joining() {
            if(try_advance_state(pipeline_state, PipelineState::SPAWNING)) {
                std::unique_lock<std::shared_mutex> lock(probe_queue_lock);

                JoinResult concatenated_probe(69);
                for(JoinResult& probe_slot : probe_queue_slots)
                    JoinResult::append_results(concatenated_probe, std::move(probe_slot));

                spawn_probe_tasks(std::move(concatenated_probe));
                try_advance_state(pipeline_state, PipelineState::JOINING);
            }
        }

        void advance_to_finished() {
            if(try_advance_state(pipeline_state, PipelineState::FINISHED)) {}
            else
                assert(false);
        }

        JoinResult construct_join_result() override {
            return JoinResult(join_metadata.scan_id_to_col.size(), next_join_col_type);
        }

    protected:
        unchained_table<T> hash_table;
        Partitions partition_result;
        DataType next_join_col_type;
};

template <typename T, typename N>
class RootDispatcherState : public JoinDispatcherState<T, N> {
    public:
        RootDispatcherState(std::shared_ptr<JoinDispatcher> parent, bool data_for_probe, ExecutionInstance& instance_in,
            size_t nextjoincol, int priority)
                : JoinDispatcherState<T, N>(std::move(parent), data_for_probe, instance_in, nextjoincol, priority) {
            this->col_load_tasks_registered = this->instance.dispatcher_state.num_deferred_tasks();
            columnar_table_slots.resize(TaskQueue::get_instance()->get_num_workers());
        }

        void probe_task_done(ColumnarTable& columnar_table) override {
            assert(this->probe_tasks_registered > 0);
            assert(worker_id >= 0);

            if (columnar_table.num_rows != 0)
                columnar_append(this->columnar_table_slots[worker_id].table, columnar_table);

            if(atomic_inc_and_cmp(this->probe_tasks_finished, this->probe_tasks_registered)
                    && this->spawned_all_probe_tasks.load(std::memory_order_acquire)) {
                this->advance_to_finished();
                this->post_probe_hook();
                for(ColumnarTableWrapper& col_table : this->columnar_table_slots)
                    columnar_append(this->instance.result, col_table.table);
                wait_object.notify_all();
            }
        }

        void col_load_task_done() override {
            assert(this->col_load_tasks_registered > 0);

            if(atomic_inc_and_cmp(this->col_load_tasks_finished, this->col_load_tasks_registered)
                    && this->pipeline_state == PipelineState::WAIT_COLUMNS){
                        this->advance_to_joining();
                }

        }

        void wait() override {
            wait_object.wait();
        }

    protected:
        void spawn_probe_tasks(JoinResult probe_data) override {
            this->template spawn_probe_tasks_common<ProbingTaskRoot, DummyProbingTaskRoot>(std::move(probe_data));
        }

        void pre_probe_hook() override {
            this->instance.result.columns.reserve(this->join_metadata.col_seq.size());
            for (auto& col_el : this->join_metadata.col_seq) {
                assert(col_el.table_id < this->instance.plan.inputs.size());
                assert(col_el.column_id < this->instance.plan.inputs[col_el.table_id].columns.size());
                const Column& origin = this->instance.plan.inputs[col_el.table_id].columns[col_el.column_id];
                this->instance.result.columns.emplace_back(origin.type);
            }

            for(int i = 0; i < TaskQueue::get_instance()->get_num_workers(); i++) {
                columnar_table_slots[i].table.columns.reserve(this->join_metadata.col_seq.size());
                for (auto& col_el : this->join_metadata.col_seq) {
                    const Column& origin = this->instance.plan.inputs[col_el.table_id].columns[col_el.column_id];
                    columnar_table_slots[i].table.columns.emplace_back(origin.type);
                }
            }
        }

        JoinResult construct_join_result() override {
            return JoinResult();
        }

    private:
        WaitObject wait_object;
        std::vector<ColumnarTableWrapper> columnar_table_slots;
};
