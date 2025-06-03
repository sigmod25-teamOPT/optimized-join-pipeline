#include <hardware.h>
#include <iostream>
#include <plan.h>
#include <table.h>
#include <fmt/core.h>
#include <data_column.h>
#include <cassert>
#include <unordered_set>
#include <defs.h>
#include <task_queue.h>
#include <unistd.h>
#include <inter_result.h>
#include <join_state.h>
#include <unchained_table.h>
#include <hardware_parse.h>

#define dispatch_join(join_dispatcher, dispatcher_class, join_field_type, next_join_field_type, depth)      \
    do {                                                                                                    \
        switch (join_field_type) {                                                                          \
            case DataType::INT32:                                                                           \
                switch (next_join_field_type) {                                                             \
                    case DataType::INT32:                                                                   \
                        join_dispatcher = std::static_pointer_cast<JoinDispatcher>(                         \
                            std::make_shared<dispatcher_class<int32_t, int32_t>>(                           \
                                parent_dispatcher, data_for_probe, instance, nextjoincol, depth             \
                            )                                                                               \
                        );                                                                                  \
                        break;                                                                              \
                    case DataType::INT64:                                                                   \
                        join_dispatcher = std::static_pointer_cast<JoinDispatcher>(                         \
                            std::make_shared<dispatcher_class<int32_t, int64_t>>(                           \
                                parent_dispatcher, data_for_probe, instance, nextjoincol, depth             \
                            )                                                                               \
                        );                                                                                  \
                        break;                                                                              \
                    default:                                                                                \
                        assert(false);                                                                      \
                        break;                                                                              \
                }                                                                                           \
                break;                                                                                      \
            case DataType::INT64:                                                                           \
                switch (next_join_field_type) {                                                             \
                    case DataType::INT32:                                                                   \
                        join_dispatcher = std::static_pointer_cast<JoinDispatcher>(                         \
                            std::make_shared<dispatcher_class<int64_t, int32_t>>(                           \
                                parent_dispatcher, data_for_probe, instance, nextjoincol, depth             \
                            )                                                                               \
                        );                                                                                  \
                        break;                                                                              \
                    case DataType::INT64:                                                                   \
                        join_dispatcher = std::static_pointer_cast<JoinDispatcher>(                         \
                            std::make_shared<dispatcher_class<int64_t, int64_t>>(                           \
                                parent_dispatcher, data_for_probe, instance, nextjoincol, depth             \
                            )                                                                               \
                        );                                                                                  \
                        break;                                                                              \
                    default:                                                                                \
                        assert(false);                                                                      \
                        break;                                                                              \
                }                                                                                           \
                break;                                                                                      \
            case DataType::FP64:                                                                            \
                assert(false);                                                                              \
                break;                                                                                      \
            case DataType::VARCHAR:                                                                         \
                assert(false);                                                                              \
                break;                                                                                      \
        }                                                                                                   \
    } while (0)

namespace Contest {

JoinMetadata execute_impl(ExecutionInstance& instance, size_t node_id, size_t nextjoincol,
    std::shared_ptr<JoinDispatcher> parent_dispatcher, bool data_for_probe, int depth);

JoinMetadata execute_join(ExecutionInstance& instance, const JoinNode& join, const Attributes& output_attrs, size_t nextjoincol,
        std::shared_ptr<JoinDispatcher> parent_dispatcher, bool data_for_probe, int depth) {
    const PlanNode& left_node = instance.plan.nodes[join.left];
    const PlanNode& right_node = instance.plan.nodes[join.right];

    const Attributes& left_types = left_node.output_attrs;
    const Attributes& right_types = right_node.output_attrs;

    DataType join_field_type = join.build_left ? std::get<1>(left_types[join.left_attr]) :
        std::get<1>(right_types[join.right_attr]);
    DataType next_join_field_type = nextjoincol != -1 ? std::get<1>(output_attrs[nextjoincol]) : DataType::INT32;

    std::shared_ptr<JoinDispatcher> join_dispatcher;

    if(nextjoincol == -1ull) {
        dispatch_join(join_dispatcher, RootDispatcherState, join_field_type, next_join_field_type, depth);
    } else {
        dispatch_join(join_dispatcher, JoinDispatcherState, join_field_type, next_join_field_type, depth);
    }

    size_t prio_left = join.build_left ? 100 : 1;
    size_t prio_right = join.build_left ? 1 : 100;

    JoinMetadata left = execute_impl(instance, join.left, join.left_attr, join_dispatcher, !join.build_left, depth + prio_left);
    JoinMetadata right = execute_impl(instance, join.right, join.right_attr, join_dispatcher, join.build_left, depth + prio_right);

    JoinMetadata& build_side = join.build_left ? left : right;
    JoinMetadata& probe_side = join.build_left ? right : left;

    std::vector<ColumnElement> concatenated_col_seq, col_seq;
    concat_vectors(concatenated_col_seq, left.col_seq, right.col_seq);
    for (auto [col_idx, _]: output_attrs)
        col_seq.emplace_back(std::move(concatenated_col_seq[col_idx]));

    assert(col_seq.size() == output_attrs.size());

    std::vector<uint8_t> scan_id_to_col;
    concat_vectors(scan_id_to_col, build_side.scan_id_to_col, probe_side.scan_id_to_col);

    JoinMetadata current_join_metadata(std::move(col_seq), std::move(scan_id_to_col));
    join_dispatcher->set_join_metadata(current_join_metadata); // DO NOT USE std::move()!!!

    if(nextjoincol == -1) {
        for (auto& context : instance.initial_tasks)
            context.dispatcher->input_data_ready(std::move(context.output), context.data_for_probe, true);

        instance.dispatcher_state.start_deferred_tasks(join_dispatcher);
        join_dispatcher->wait();
    }

    return current_join_metadata;
}

JoinMetadata execute_scan(ExecutionInstance& instance, const ScanNode& scan, const Attributes& output_attrs, size_t nextjoincol,
        std::shared_ptr<JoinDispatcher> parent_dispatcher, bool data_for_probe, int depth) {
    assert(nextjoincol != -1ull);
    uint8_t scan_id = instance.cur_scan_id++;
    std::vector<ColumnElement> new_col_seq;
    new_col_seq.reserve(output_attrs.size());

    for(size_t i = 0; i < output_attrs.size(); i++) {
        new_col_seq.emplace_back(
            ColumnElement(
                static_cast<uint8_t>(scan.base_table_id),
                static_cast<uint8_t>(std::get<0>(output_attrs[i])),
                scan_id
            )
        );
    }

    int row_count = instance.plan.inputs[scan.base_table_id].num_rows;
    DataColumn& col_to_pass = instance.column_store[scan.base_table_id].get_column(std::get<0>(output_attrs[nextjoincol]));

    JoinResult output(col_to_pass);
    instance.initial_tasks.emplace_back(std::move(parent_dispatcher), std::move(output), data_for_probe);

    return JoinMetadata(scan_id, std::move(new_col_seq));
}

JoinMetadata execute_impl(ExecutionInstance& instance, size_t node_id, size_t nextjoincol,
        std::shared_ptr<JoinDispatcher> parent_dispatcher, bool data_for_probe, int depth) {
    assert(instance.column_store.size() == instance.plan.inputs.size());

    auto& node = instance.plan.nodes[node_id];
    return std::visit(
        [&](const auto& node_p) {
            using T = std::decay_t<decltype(node_p)>;
            if constexpr (std::is_same_v<T, JoinNode>)
                return execute_join(instance, node_p, node.output_attrs, nextjoincol, parent_dispatcher, data_for_probe, depth);
            else
                return execute_scan(instance, node_p, node.output_attrs, nextjoincol, parent_dispatcher, data_for_probe, depth);
        },
        node.data);
}

#include <unistd.h>

ColumnarTable execute(const Plan& plan, [[maybe_unused]] void* context) {
    ExecutionInstance instance(plan, (TaskQueue*) context);
    execute_impl(instance, instance.plan.root, -1ull, nullptr, false, 0);
    return std::move(instance.result);
}

void* build_context() {
    return (void*) new TaskQueue(P_SPC__CORE_COUNT);
}

void destroy_context([[maybe_unused]] void* context) {
    TaskQueue* task_queue = (TaskQueue*) context;
    delete task_queue;
}

} // namespace Contest
