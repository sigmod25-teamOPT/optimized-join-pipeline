#pragma once
#pragma clang diagnostic ignored "-Wnull-dereference"

#include <defs.h>
#include <vector>
#include <column_wrapper.h>
#include <join_result.h>
#include <execution_instance.h>
#include <inter_result.h>
#include <inter_column.h>
#include <task.h>
#include <unchained_table.h>
#include <iostream>
#include <pages_feeder.h>

class JoinDispatcher;

#define CHECK_VALID(type) CHECK_VALID_##type
#define CHECK_VALID_on if(!p_column.is_valid(idx)) continue;
#define CHECK_VALID_off

#define HASH_VEC_REC (rid_vec){ .data = (rid_t*)&hash_idx, .size = 1 }

#define PROBE_LOOP_SCAN(build_recM)                                                                                 \
    if(p_column.pagenated()) {                                                                                      \
        rid_vec proxy_vec = { .data = (rid_t*)&idx, .size = 1 };                                                    \
        DataColumn::PageIterator it = p_column.r_page_iterator(first_page * CAP_PER_PAGE, last_record - 1);         \
        for (; !it.r_end(); --it) {                                                                                 \
            T key = it.retrieve_value<T>();                                                                         \
            size_t veclen = 0;                                                                                      \
            Entry<T>* vec = hash_table.lookup(key, veclen);                                                         \
            if (vec == nullptr) continue;                                                                           \
            idx = it.get_index();                                                                                   \
            for (size_t i = 0; i < veclen; i++) {                                                                   \
                if (vec[i].key == key) {                                                                            \
                    super_id hash_idx = vec[i].row_id;                                                              \
                    add_result(build_recM, proxy_vec, next_join_col, table_idx);                                    \
                }                                                                                                   \
            }                                                                                                       \
        }\
                                                                                                                     \
    } else {                                                                                                            \
        for (idx = last_record - 1; (int64_t)idx >= (int64_t)(first_page * CAP_PER_PAGE); idx--) {                      \
            if(!p_column.is_valid(idx)) continue;                                                                       \
            T key = p_column.retrieve_value<T>(idx);                                                                    \
            size_t veclen = 0;                                                                                          \
            Entry<T>* vec = hash_table.lookup(key, veclen);                                                             \
            if (vec == nullptr) continue;                                                                               \
            for (size_t i = 0; i < veclen; i++) {                                                                       \
                if (vec[i].key == key) {                                                                                \
                    super_id hash_idx = vec[i].row_id;                                                                  \
                    add_result(build_recM, proxy_vec, next_join_col, table_idx);                                        \
                }                                                                                                       \
            }                                                                                                           \
        }                                                                                                               \
    }

#define PROBE_LOOP_INTER(build_recM, probe_recM)                                                                        \
    for(super.offset.page = first_page+page_count-1; (int32_t)super.offset.page >= (int32_t)first_page; super.offset.page--){    \
        uint32_t row_count = probe_side->result_rowids.pages[super.offset.page].get_size();                             \
        assert(row_count);                                                                                              \
        for(super.offset.offset=row_count-1; (int32_t)super.offset.offset >= 0; super.offset.offset--){                 \
            T key = p_column.retrieve_value<T>(super);                                                                  \
            size_t veclen = 0;                                                                                          \
            Entry<T>* vec = hash_table.lookup(key, veclen);                                                             \
            if (vec == nullptr) continue;                                                                               \
            for (size_t i = 0; i < veclen; i++) {                                                                       \
                if (vec[i].key == key) {                                                                                \
                    super_id hash_idx = vec[i].row_id;                                                                  \
                    add_result(build_recM, probe_recM, next_join_col, table_idx);                                       \
                }                                                                                                       \
            }                                                                                                           \
        }                                                                                                               \
    }

#define FILTER_LOOP_SCAN(build_recM, other)                                                                             \
    if(p_column.pagenated()) {                                                                                          \
        DataColumn::PageIterator it = p_column.r_page_iterator(first_page * CAP_PER_PAGE, last_record - 1);             \
        for (; !it.r_end(); --it) {                                                                                     \
            T key = it.retrieve_value<T>();                                                                             \
            if (key != other) continue;                                                                                 \
            idx = it.get_index();                                                                                       \
            add_result(build_recM, proxy_vec, next_join_col, table_idx);                                                \
        }                                                                                                               \
    } else {                                                                                                            \
        for (idx = last_record - 1; (int64_t)idx >= (int64_t)(first_page * CAP_PER_PAGE); idx--) {                      \
            if(!p_column.is_valid(idx)) continue;                                                                       \
            T key = p_column.retrieve_value<T>(idx);                                                                    \
            if (key != other) continue;                                                                                 \
            add_result(build_recM, proxy_vec, next_join_col, table_idx);                                                \
        }                                                                                                               \
    }

#define FILTER_LOOP_INTER(build_recM, probe_recM, other)                                                                        \
    for(super.offset.page = first_page+page_count-1; (int32_t)super.offset.page >= (int32_t)first_page; super.offset.page--){    \
        uint32_t row_count = probe_side->result_rowids.pages[super.offset.page].get_size();                             \
        assert(row_count);                                                                                              \
        for(super.offset.offset=row_count-1; (int32_t)super.offset.offset >= 0; super.offset.offset--){                 \
            T key = p_column.retrieve_value<T>(super);                                                                  \
            if (key != other) continue;                                                                                 \
            add_result(build_recM, probe_recM, next_join_col, table_idx);                                                  \
        }                                                                                                               \
    }

template <typename T, typename N>
class ProbingTask : public Task {
    public:
        ProbingTask(const unchained_table<T>& hash_table,
                    std::shared_ptr<JoinDispatcher> join_dispatcher,
                    ExecutionInstance& instance,
                    JoinMetadata& join_metadata,
                    const JoinResult& build_side,
                    std::shared_ptr<JoinResult> probe_side,
                    size_t output_colidx,
                    int priority,
                    size_t feeder_idx,
                    size_t feeder_cnt)
            : Task(priority),
              hash_table(hash_table),
              join_dispatcher(join_dispatcher),
              instance(instance),
              join_metadata(join_metadata),
              build_side(build_side),
              probe_side(probe_side),
              output_colidx(output_colidx),
              results(join_metadata.scan_id_to_col.size(), reduce_template<N>()),
              feeder_idx(feeder_idx),
              feeder_cnt(feeder_cnt),
              pages_feeders(probe_side->pages_feeders) {}

    protected:
        void run_task() override {
            assert(output_colidx != -1ull);
            const DataColumn& next_join_col = instance.column_store[join_metadata.col_seq[output_colidx].table_id].
                    get_column(join_metadata.col_seq[output_colidx].column_id);
            size_t table_idx = join_metadata.get_column_index_in_results(output_colidx);
            size_t page_count = 1;
            size_t first_page = -1ull;
            size_t total_pages = probe_side->is_from_scan() ? probe_side->result_col_scan.get_page_count()
                : probe_side->result_col_join.pages.size();

            assert(!probe_side->is_empty());

            for (size_t feeder_offset = 0; feeder_offset < feeder_cnt; feeder_offset++) {
                size_t local_feeder_idx = (feeder_idx + feeder_offset) % feeder_cnt;
                while ((first_page = pages_feeders[local_feeder_idx].next()) != -1ull) {
                    if (probe_side->is_from_scan()) {
                        size_t idx = 0;
                        rid_vec proxy_vec = { .data = (rid_t*)&idx, .size = 1 };
                        size_t last_record = std::min((first_page + (page_count)) * CAP_PER_PAGE, probe_side->get_output_size());
                        assert(last_record <= probe_side->get_output_size());
                        const DataColumn& p_column = probe_side->result_col_scan;

                        if(build_side.get_output_size() == 1) {
                            T other_key = 0;
                            super_id hash_idx = {{0}};

                            if (build_side.is_from_scan()) {
                                other_key = build_side.result_col_scan.retrieve_value<T>(0);
                                FILTER_LOOP_SCAN(HASH_VEC_REC, other_key);
                            } else {
                                other_key = build_side.result_col_join.retrieve_value<T>(hash_idx);
                                FILTER_LOOP_SCAN(build_side.result_rowids[hash_idx], other_key);
                            }
                        } else {
                            if (build_side.is_from_scan()) {
                                PROBE_LOOP_SCAN(HASH_VEC_REC);
                            } else {
                                PROBE_LOOP_SCAN(build_side.result_rowids[hash_idx]);
                            }
                        }

                    } else {
                        const IntermediateColumn& p_column = probe_side->result_col_join;
                        assert(first_page + page_count <= probe_side->result_rowids.pages.size());
                        super_id super = { .offset = { .page = 0, .offset = 0 } };

                        if(build_side.get_output_size() == 1) {
                            T other_key = 0;
                            super_id hash_idx = {{0}};

                            if (build_side.is_from_scan()) {
                                other_key = build_side.result_col_scan.retrieve_value<T>(0);
                                FILTER_LOOP_INTER(HASH_VEC_REC, probe_side->result_rowids[super], other_key);
                            } else {
                                other_key = build_side.result_col_join.retrieve_value<T>(hash_idx);
                                FILTER_LOOP_INTER(build_side.result_rowids[hash_idx], probe_side->result_rowids[super], other_key);
                            }
                        }else {
                            if (build_side.is_from_scan()) {
                                PROBE_LOOP_INTER(HASH_VEC_REC, probe_side->result_rowids[super]);
                            } else {
                                PROBE_LOOP_INTER(build_side.result_rowids[hash_idx], probe_side->result_rowids[super]);
                            }
                        }
                    }
                }
            }

        }

        void post_run_clarity() override;

    private:
        inline void add_result(const rid_vec& build_rec, const rid_vec& probe_rec,
                const DataColumn& next_join_col, size_t table_idx) {
            rid_t* new_rec = table_idx < build_rec.size ? build_rec.data : probe_rec.data-build_rec.size;
            if (next_join_col.is_valid(new_rec[table_idx])){

                results.result_col_join.insert<N>(next_join_col.retrieve_value<N>(new_rec[table_idx]));
                results.result_rowids.add_cat_rows(build_rec, probe_rec);
            }
        }

        size_t feeder_idx, feeder_cnt;
        PagesFeeder *pages_feeders;
        const unchained_table<T>& hash_table;
        std::shared_ptr<JoinDispatcher> join_dispatcher;
        ExecutionInstance& instance;
        JoinMetadata join_metadata;
        const JoinResult& build_side;
        std::shared_ptr<JoinResult> probe_side;
        size_t output_colidx;

        JoinResult results;
        std::vector<ColumnWrapper> wraps;
};

template <typename T, typename N>
class ProbingTaskRoot : public Task {
    public:
        ProbingTaskRoot(const unchained_table<T>& hash_table,
                    std::shared_ptr<JoinDispatcher> join_dispatcher,
                    ExecutionInstance& instance,
                    JoinMetadata& join_metadata,
                    const JoinResult& build_side,
                    std::shared_ptr<JoinResult> probe_side,
                    size_t output_colidx,
                    int priority,
                    size_t feeder_idx,
                    size_t feeder_cnt)
                : Task(priority),
                hash_table(hash_table),
                join_dispatcher(join_dispatcher),
                instance(instance),
                join_metadata(join_metadata),
                build_side(build_side),
                probe_side(probe_side),
                output_colidx(output_colidx),
                feeder_idx(feeder_idx),
                feeder_cnt(feeder_cnt),
                pages_feeders(probe_side->pages_feeders) {
            columnar_table.columns.reserve(this->join_metadata.col_seq.size());
            this->wraps.reserve(this->join_metadata.col_seq.size());
            for (auto& col_el : this->join_metadata.col_seq) {
                assert(col_el.table_id < this->instance.plan.inputs.size());
                assert(col_el.column_id < this->instance.plan.inputs[col_el.table_id].columns.size());
                const Column& origin = this->instance.plan.inputs[col_el.table_id].columns[col_el.column_id];
                columnar_table.columns.emplace_back(origin.type);
                this->wraps.emplace_back(origin.type, columnar_table.columns.back(), origin);
            }
        }

    protected:
        void run_task() override {
            assert(output_colidx == -1ull);
            const DataColumn& next_join_col = NULL_DATA_COLUMN;
            size_t table_idx = -1ull;
            size_t page_count = 1;
            size_t first_page = -1ull;
            size_t total_pages = probe_side->is_from_scan() ? probe_side->result_col_scan.get_page_count()
                : probe_side->result_col_join.pages.size();

            assert(!probe_side->is_empty());

            for (size_t feeder_offset = 0; feeder_offset < feeder_cnt; feeder_offset++) {
                size_t local_feeder_idx = (feeder_idx + feeder_offset) % feeder_cnt;
                while ((first_page = pages_feeders[local_feeder_idx].next()) != -1ull) {
                    assert(first_page != -1ull);

                    if (probe_side->is_from_scan()) {
                        size_t idx = 0;
                        rid_vec proxy_vec = { .data = (rid_t*)&idx, .size = 1 };
                        size_t last_record = std::min((first_page + (page_count)) * CAP_PER_PAGE, probe_side->get_output_size());
                        assert(last_record <= probe_side->get_output_size());
                        const DataColumn& p_column = probe_side->result_col_scan;

                        if(build_side.get_output_size() == 1) {
                            T other_key = 0;
                            super_id hash_idx = {{0}};

                            if (build_side.is_from_scan()) {
                                other_key = build_side.result_col_scan.retrieve_value<T>(0);
                                FILTER_LOOP_SCAN(HASH_VEC_REC, other_key);
                            } else {
                                other_key = build_side.result_col_join.retrieve_value<T>(hash_idx);
                                FILTER_LOOP_SCAN(build_side.result_rowids[hash_idx], other_key);
                            }
                        } else {
                            if (build_side.is_from_scan()) {
                                PROBE_LOOP_SCAN(HASH_VEC_REC);
                            } else {
                                PROBE_LOOP_SCAN(build_side.result_rowids[hash_idx]);
                            }
                        }

                    } else {
                        const IntermediateColumn& p_column = probe_side->result_col_join;
                        assert(first_page + page_count <= probe_side->result_rowids.pages.size());
                        super_id super = { .offset = { .page = 0, .offset = 0 } };

                        if(build_side.get_output_size() == 1) {
                            T other_key = 0;
                            super_id hash_idx = {{0}};

                            if (build_side.is_from_scan()) {
                                other_key = build_side.result_col_scan.retrieve_value<T>(0);
                                FILTER_LOOP_INTER(HASH_VEC_REC, probe_side->result_rowids[super], other_key);
                            } else {
                                other_key = build_side.result_col_join.retrieve_value<T>(hash_idx);
                                FILTER_LOOP_INTER(build_side.result_rowids[hash_idx], probe_side->result_rowids[super], other_key);
                            }
                        }else {
                            if (build_side.is_from_scan()) {
                                PROBE_LOOP_INTER(HASH_VEC_REC, probe_side->result_rowids[super]);
                            } else {
                                PROBE_LOOP_INTER(build_side.result_rowids[hash_idx], probe_side->result_rowids[super]);
                            }
                        }
                    }
                }
            }


            finalize();
        }

        void post_run_clarity() override;

    private:
        inline void add_result(const rid_vec& left_rec, const rid_vec& right_rec,
            const DataColumn& next_join_col [[ maybe_unused ]], size_t table_idx [[ maybe_unused ]]) {
            assert(wraps.size() == join_metadata.col_seq.size());

            for (size_t i = 0 ; i < join_metadata.col_seq.size(); i++) {
                ColumnElement col_el = join_metadata.get_column_element(i);
                DataColumn& col = instance.column_store[col_el.table_id].get_column(col_el.column_id);
                DataType type = col.get_type();
                assert(type == wraps[i].get_type());
                size_t table_idx = join_metadata.get_column_index_in_results(i);
                size_t row = table_idx < left_rec.size ? left_rec.data[table_idx] : right_rec.data[table_idx-left_rec.size];
                if (col.is_valid(row)) {
                    wraps[i].insert(col.retrieve_value_ptr(row));
                }
                else
                    wraps[i].insert_null();
            }

            columnar_table.num_rows++;
        }

        void finalize() {
            for (size_t i = 0; i < this->wraps.size(); i++)
                this->wraps[i].finalize();
        }

        size_t feeder_idx, feeder_cnt;
        PagesFeeder *pages_feeders;
        const unchained_table<T>& hash_table;
        std::shared_ptr<JoinDispatcher> join_dispatcher;
        ExecutionInstance& instance;
        JoinMetadata join_metadata;
        const JoinResult& build_side;
        std::shared_ptr<JoinResult> probe_side;
        size_t output_colidx;
        std::vector<ColumnWrapper> wraps;

        ColumnarTable columnar_table;
};

class DummyProbingTask : public Task {
    public:
        DummyProbingTask(std::shared_ptr<JoinDispatcher> join_dispatcher)
            : Task(0), join_dispatcher(std::move(join_dispatcher)) {}

        protected:
            void run_task() override {
                return;
            }

            void post_run_clarity() override;
    private:
        std::shared_ptr<JoinDispatcher> join_dispatcher;
};

class DummyProbingTaskRoot : public Task {
    public:
        DummyProbingTaskRoot(std::shared_ptr<JoinDispatcher> join_dispatcher)
            :Task(0), join_dispatcher(std::move(join_dispatcher)) {}

        protected:
            void run_task() override {
                return;
            }

            void post_run_clarity() override;

    private:
        std::shared_ptr<JoinDispatcher> join_dispatcher;
};


extern template void ProbingTask<int32_t, int32_t>::post_run_clarity();
extern template void ProbingTask<int32_t, int64_t>::post_run_clarity();
extern template void ProbingTask<int64_t, int32_t>::post_run_clarity();
extern template void ProbingTask<int64_t, int64_t>::post_run_clarity();

extern template void ProbingTaskRoot<int32_t, int32_t>::post_run_clarity();
extern template void ProbingTaskRoot<int32_t, int64_t>::post_run_clarity();
extern template void ProbingTaskRoot<int64_t, int32_t>::post_run_clarity();
extern template void ProbingTaskRoot<int64_t, int64_t>::post_run_clarity();
