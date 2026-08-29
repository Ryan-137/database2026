#pragma once

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "executor_abstract.h"

class AggregateExecutor : public AbstractExecutor {
private:
    struct Group {
        std::vector<RmRecord> rows;
    };

    struct EvalValue {
        ColType type;
        int len;
        std::vector<char> data;
    };

    struct ScalarAccumulator {
        size_t count{0};
        long long int_sum{0};
        double float_sum{0};
        std::vector<char> best;
        bool has_value{false};
    };

    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<SelectExpr> select_exprs_;
    std::vector<TabCol> group_cols_;
    std::vector<HavingCondition> having_conds_;
    std::vector<ColMeta> cols_;
    size_t len_ = 0;
    std::vector<RmRecord> records_;
    size_t pos_ = 0;

    bool can_stream_scalar_aggregate() const {
        static const bool enabled = [] {
            const char *value = std::getenv("RMDB_ENABLE_STREAMING_AGG");
            // 默认启用；显式设为 0 可退回原聚合路径。
            return value == nullptr || strcmp(value, "0") != 0;
        }();
        if (!enabled || !group_cols_.empty() || !having_conds_.empty()) {
            return false;
        }
        // 多聚合表达式保留原批量分组路径；流式路径只处理单聚合查询。
        return select_exprs_.size() == 1 && select_exprs_.front().is_aggregate;
    }

    const ColMeta &input_col(const TabCol &target) const {
        return *get_col(prev_->cols(), target);
    }

    EvalValue eval_expr(const SelectExpr &expr, const Group &group) const {
        if (!expr.is_aggregate) {
            const auto &col = input_col(expr.col);
            EvalValue value{col.type, col.len, std::vector<char>(col.len, 0)};
            if (!group.rows.empty()) {
                memcpy(value.data.data(), group.rows.front().data + col.offset, col.len);
            }
            return value;
        }

        if (expr.agg_func == AGG_COUNT) {
            EvalValue value{TYPE_INT, static_cast<int>(sizeof(int)), std::vector<char>(sizeof(int), 0)};
            int count = static_cast<int>(group.rows.size());
            memcpy(value.data.data(), &count, sizeof(count));
            return value;
        }

        const auto &col = input_col(expr.col);
        ColType result_type = expr.agg_func == AGG_AVG ? TYPE_FLOAT : col.type;
        int result_len = result_type == TYPE_STRING ? col.len : static_cast<int>(sizeof(float));
        if (result_type == TYPE_INT) result_len = sizeof(int);
        EvalValue value{result_type, result_len, std::vector<char>(result_len, 0)};

        if (expr.agg_func == AGG_MAX || expr.agg_func == AGG_MIN) {
            if (!group.rows.empty()) {
                const char *best = group.rows.front().data + col.offset;
                for (size_t i = 1; i < group.rows.size(); ++i) {
                    const char *candidate = group.rows[i].data + col.offset;
                    int cmp = CompareValue(candidate, best, col.type, col.len);
                    if ((expr.agg_func == AGG_MAX && cmp > 0) || (expr.agg_func == AGG_MIN && cmp < 0)) {
                        best = candidate;
                    }
                }
                memcpy(value.data.data(), best, col.len);
            }
            return value;
        }

        if (col.type == TYPE_INT) {
            long long sum = 0;
            for (const auto &row : group.rows) sum += *reinterpret_cast<const int *>(row.data + col.offset);
            if (expr.agg_func == AGG_AVG) {
                float result = group.rows.empty() ? 0.0f : static_cast<float>(sum) / group.rows.size();
                memcpy(value.data.data(), &result, sizeof(result));
            } else {
                int result = static_cast<int>(sum);
                memcpy(value.data.data(), &result, sizeof(result));
            }
        } else {
            double sum = 0;
            for (const auto &row : group.rows) sum += *reinterpret_cast<const float *>(row.data + col.offset);
            float result = expr.agg_func == AGG_AVG && !group.rows.empty()
                               ? static_cast<float>(sum / group.rows.size())
                               : static_cast<float>(sum);
            memcpy(value.data.data(), &result, sizeof(result));
        }
        return value;
    }

    bool passes_having(const Group &group) const {
        for (const auto &cond : having_conds_) {
            auto lhs = eval_expr(cond.lhs, group);
            auto rhs_expr = cond.is_rhs_val ? EvalValue{} : eval_expr(cond.rhs_expr, group);
            const char *rhs = cond.is_rhs_val ? cond.rhs.raw->data : rhs_expr.data.data();
            int cmp = CompareValue(lhs.data.data(), rhs, lhs.type, lhs.len);
            if (!CheckOp(cmp, cond.op)) return false;
        }
        return true;
    }

    void update_scalar_accumulator(const SelectExpr &expr, const ColMeta *col,
                                   ScalarAccumulator &state, const RmRecord &row) const {
        state.count++;
        if (expr.agg_func == AGG_COUNT) {
            return;
        }

        const char *value = row.data + col->offset;
        if (expr.agg_func == AGG_MIN || expr.agg_func == AGG_MAX) {
            if (!state.has_value) {
                state.best.assign(value, value + col->len);
                state.has_value = true;
                return;
            }
            int cmp = CompareValue(value, state.best.data(), col->type, col->len);
            if ((expr.agg_func == AGG_MIN && cmp < 0) || (expr.agg_func == AGG_MAX && cmp > 0)) {
                memcpy(state.best.data(), value, col->len);
            }
            return;
        }

        if (col->type == TYPE_INT) {
            state.int_sum += *reinterpret_cast<const int *>(value);
        } else {
            state.float_sum += *reinterpret_cast<const float *>(value);
        }
    }

    EvalValue finalize_scalar_accumulator(const SelectExpr &expr, const ScalarAccumulator &state) const {
        if (expr.agg_func == AGG_COUNT) {
            EvalValue value{TYPE_INT, static_cast<int>(sizeof(int)), std::vector<char>(sizeof(int), 0)};
            int count = static_cast<int>(state.count);
            memcpy(value.data.data(), &count, sizeof(count));
            return value;
        }

        const auto &col = input_col(expr.col);
        ColType result_type = expr.agg_func == AGG_AVG ? TYPE_FLOAT : col.type;
        int result_len = result_type == TYPE_STRING ? col.len : static_cast<int>(sizeof(float));
        if (result_type == TYPE_INT) result_len = sizeof(int);
        EvalValue value{result_type, result_len, std::vector<char>(result_len, 0)};

        if (expr.agg_func == AGG_MIN || expr.agg_func == AGG_MAX) {
            if (state.has_value) {
                memcpy(value.data.data(), state.best.data(), col.len);
            }
            return value;
        }

        if (col.type == TYPE_INT) {
            if (expr.agg_func == AGG_AVG) {
                float result = state.count == 0 ? 0.0f : static_cast<float>(state.int_sum) / state.count;
                memcpy(value.data.data(), &result, sizeof(result));
            } else {
                int result = static_cast<int>(state.int_sum);
                memcpy(value.data.data(), &result, sizeof(result));
            }
        } else {
            float result = expr.agg_func == AGG_AVG && state.count != 0
                               ? static_cast<float>(state.float_sum / state.count)
                               : static_cast<float>(state.float_sum);
            memcpy(value.data.data(), &result, sizeof(result));
        }
        return value;
    }

    void run_scalar_streaming() {
        const SelectExpr &expr = select_exprs_.front();
        const ColMeta *col = expr.agg_func == AGG_COUNT ? nullptr : &input_col(expr.col);
        ScalarAccumulator state;
        for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
            std::unique_ptr<RmRecord> owned_row;
            const RmRecord *row = prev_->CurrentOrNext(&owned_row);
            update_scalar_accumulator(expr, col, state, *row);
        }

        RmRecord output(static_cast<int>(len_));
        auto value = finalize_scalar_accumulator(expr, state);
        memcpy(output.data + cols_.front().offset, value.data.data(), cols_.front().len);
        records_.emplace_back(std::move(output));
    }

public:
    AggregateExecutor(std::unique_ptr<AbstractExecutor> prev, std::vector<SelectExpr> select_exprs,
                      std::vector<TabCol> group_cols, std::vector<HavingCondition> having_conds)
        : prev_(std::move(prev)), select_exprs_(std::move(select_exprs)),
          group_cols_(std::move(group_cols)), having_conds_(std::move(having_conds)) {
        int offset = 0;
        for (const auto &expr : select_exprs_) {
            ColMeta col;
            col.tab_name = "";
            col.name = expr.output_name;
            col.offset = offset;
            col.index = false;
            if (!expr.is_aggregate) {
                const auto &source = input_col(expr.col);
                col.type = source.type;
                col.len = source.len;
            } else if (expr.agg_func == AGG_COUNT) {
                col.type = TYPE_INT;
                col.len = sizeof(int);
            } else if (expr.agg_func == AGG_AVG) {
                col.type = TYPE_FLOAT;
                col.len = sizeof(float);
            } else {
                const auto &source = input_col(expr.col);
                col.type = source.type;
                col.len = source.len;
            }
            offset += col.len;
            cols_.push_back(col);
        }
        len_ = offset;
    }

    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    std::string getType() override { return "AggregateExecutor"; }

    void beginTuple() override {
        records_.clear();
        pos_ = 0;
        if (can_stream_scalar_aggregate()) {
            // 标量聚合只需单遍累加；不保留输入行，避免大扫描的深拷贝和重复遍历。
            run_scalar_streaming();
            return;
        }
        std::vector<Group> groups;
        std::unordered_map<std::string, size_t> group_index;
        if (group_cols_.empty()) groups.emplace_back();

        for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
            std::unique_ptr<RmRecord> owned_row;
            const RmRecord *row = prev_->CurrentOrNext(&owned_row);
            std::string key;
            for (const auto &group_col : group_cols_) {
                const auto &col = input_col(group_col);
                key.append(row->data + col.offset, col.len);
            }
            size_t index = 0;
            if (!group_cols_.empty()) {
                auto inserted = group_index.emplace(key, groups.size());
                if (inserted.second) groups.emplace_back();
                index = inserted.first->second;
            }
            groups[index].rows.emplace_back(*row);
        }

        for (const auto &group : groups) {
            if (!passes_having(group)) continue;
            RmRecord output(static_cast<int>(len_));
            for (size_t i = 0; i < select_exprs_.size(); ++i) {
                auto value = eval_expr(select_exprs_[i], group);
                memcpy(output.data + cols_[i].offset, value.data.data(), cols_[i].len);
            }
            records_.emplace_back(std::move(output));
        }
    }

    void nextTuple() override { ++pos_; }
    bool is_end() const override { return pos_ >= records_.size(); }
    std::unique_ptr<RmRecord> Next() override {
        return is_end() ? nullptr : std::make_unique<RmRecord>(records_[pos_]);
    }
    const RmRecord *Current() const override { return is_end() ? nullptr : &records_[pos_]; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
    Rid &rid() override { return _abstract_rid; }
};
