#pragma once

#include <plan.h>
#include <column_store.h>
#include <vector>
#include <plan_traversal.h>
#include <task_queue.h>
#include <task_column_load.h>
#include <sync_primitives.h>
#include <task_queue.h>
#include <join_result.h>

struct JoinExecutionContext {
    JoinExecutionContext(std::shared_ptr<JoinDispatcher> dispatcher_in, JoinResult output_in, bool data_for_probe_in)
        : dispatcher(std::move(dispatcher_in)), output(std::move(output_in)), data_for_probe(data_for_probe_in) {}

    std::shared_ptr<JoinDispatcher> dispatcher;
    JoinResult output;
    bool data_for_probe;
};

struct ExecutionInstance {
    ExecutionInstance(const Plan& input_plan, TaskQueue* task_queue_in) : task_queue(task_queue_in),
            plan(input_plan), cur_scan_id(0), die(false) {
        PlanStats stats(plan);

        column_store.reserve(plan.inputs.size());

        for (size_t i = 0; i < plan.inputs.size(); i++)
            column_store.emplace_back(plan.inputs[i], stats.column_use[i], dispatcher_state);

        dispatcher_state.wait_for_join_loads();
    }

    ExecutionInstance& operator=(const ExecutionInstance&) = delete;
    ExecutionInstance(ExecutionInstance&&) = delete;
    ExecutionInstance& operator=(ExecutionInstance&&) = delete;
    ExecutionInstance(const ExecutionInstance&) = delete;

    TaskQueue* task_queue;

    LoadDispatcherState dispatcher_state;
    std::vector<ColumnStore> column_store;
    const Plan& plan;
    uint8_t cur_scan_id;
    ColumnarTable result;
    bool die;

    std::vector<JoinExecutionContext> initial_tasks;
};
