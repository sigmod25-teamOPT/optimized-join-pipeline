#pragma once

#include <vector>
#include <defs.h>
#include <plan.h>

struct TraversalData {
    TraversalData(std::vector<ColumnElement> col_seq) {
        this->col_seq = std::move(col_seq);
    }

    std::vector<ColumnElement> col_seq;
};

class PlanStats {
    public:
        PlanStats(const Plan& plan);
        std::vector<std::vector<ColumnUse>> column_use;

    private:
        TraversalData traverse_join_node(const Plan& plan, const JoinNode& join, const Attributes& output_attrs);
        TraversalData traverse_scan_node(const ScanNode& scan, const Attributes& output_attrs);
        TraversalData traverse_plan(const Plan& plan, size_t node_id);
};