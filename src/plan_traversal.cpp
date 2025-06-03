#include <plan_traversal.h>

#include <stddef.h>
#include <cassert>
#include <utils.h>

PlanStats::PlanStats(const Plan& plan) {
    column_use.reserve(plan.inputs.size());
    for (const auto& table : plan.inputs)
        column_use.emplace_back(table.columns.size(), UNUSED);

    assert(column_use.size() == plan.inputs.size());
    #ifndef NDEBUG
    for (size_t i = 0; i < column_use.size(); i++)
        assert(column_use[i].size() == plan.inputs[i].columns.size());
    #endif

    traverse_plan(plan, plan.root);
}

TraversalData PlanStats::traverse_join_node(const Plan& plan, const JoinNode& join, const Attributes& output_attrs) {
    size_t left_idx = join.left;
    size_t right_idx = join.right;

    TraversalData left = traverse_plan(plan, left_idx);
    TraversalData right = traverse_plan(plan, right_idx);

    ColumnElement col_el_left = left.col_seq[join.left_attr];
    ColumnElement col_el_right = right.col_seq[join.right_attr];

    column_use[col_el_left.table_id][col_el_left.column_id] = ColumnUse::JOIN;
    column_use[col_el_right.table_id][col_el_right.column_id] = ColumnUse::JOIN;

    std::vector<ColumnElement> concatenated_col_seq, col_seq;
    concat_vectors(concatenated_col_seq, left.col_seq, right.col_seq);
    for (auto [col_idx, _]: output_attrs)
        col_seq.emplace_back(std::move(concatenated_col_seq[col_idx]));

    return TraversalData(std::move(col_seq));
}

TraversalData PlanStats::traverse_scan_node(const ScanNode& scan, const Attributes& output_attrs) {
    std::vector<ColumnElement> new_col_seq;
    new_col_seq.reserve(output_attrs.size());

    for(size_t i = 0; i < output_attrs.size(); i++) {
        new_col_seq.emplace_back(
            ColumnElement(
                static_cast<uint8_t>(scan.base_table_id),
                static_cast<uint8_t>(std::get<0>(output_attrs[i])),
                0
            )
        );

        size_t table_id = scan.base_table_id;
        size_t column_id = std::get<0>(output_attrs[i]);
        if(column_use[table_id][column_id] == ColumnUse::UNUSED)
            column_use[table_id][column_id] = ColumnUse::RESULT_ONLY;
    }

    return TraversalData(std::move(new_col_seq));
}

TraversalData PlanStats::traverse_plan(const Plan& plan, size_t node_id) {
    auto& node = plan.nodes[node_id];
    return std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, JoinNode>)
                return traverse_join_node(plan, value, node.output_attrs);
            else
                return traverse_scan_node(value, node.output_attrs);
        },
        node.data);
}