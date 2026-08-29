#pragma once

#include <algorithm>
#include <memory>
#include <vector>

#include "executor_abstract.h"

class SortExecutor : public AbstractExecutor {
private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<ColMeta> sort_cols_;
    std::vector<bool> is_desc_;
    int limit_;
    std::vector<RmRecord> tuples_;
    size_t pos_ = 0;

public:
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, const std::vector<TabCol> &sel_cols,
                 std::vector<bool> is_desc, int limit)
        : prev_(std::move(prev)), is_desc_(std::move(is_desc)), limit_(limit) {
        for (const auto &sel_col : sel_cols) sort_cols_.push_back(prev_->get_col_offset(sel_col));
    }

    size_t tupleLen() const override { return prev_->tupleLen(); }
    const std::vector<ColMeta> &cols() const override { return prev_->cols(); }
    std::string getType() override { return "SortExecutor"; }
    ColMeta get_col_offset(const TabCol &target) override { return prev_->get_col_offset(target); }

    void beginTuple() override {
        tuples_.clear();
        pos_ = 0;
        for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
            std::unique_ptr<RmRecord> owned_tuple;
            const RmRecord *tuple = prev_->CurrentOrNext(&owned_tuple);
            if (tuple != nullptr) {
                if (owned_tuple != nullptr) {
                    tuples_.emplace_back(std::move(*owned_tuple));
                } else {
                    tuples_.emplace_back(*tuple);
                }
            }
        }
        std::stable_sort(tuples_.begin(), tuples_.end(), [&](const RmRecord &lhs, const RmRecord &rhs) {
            for (size_t i = 0; i < sort_cols_.size(); ++i) {
                const auto &col = sort_cols_[i];
                int cmp = CompareValue(lhs.data + col.offset, rhs.data + col.offset, col.type, col.len);
                if (cmp != 0) return is_desc_[i] ? cmp > 0 : cmp < 0;
            }
            return false;
        });
        if (limit_ >= 0 && tuples_.size() > static_cast<size_t>(limit_)) {
            tuples_.resize(static_cast<size_t>(limit_));
        }
    }

    void nextTuple() override { ++pos_; }
    bool is_end() const override { return pos_ >= tuples_.size(); }
    std::unique_ptr<RmRecord> Next() override {
        return is_end() ? nullptr : std::make_unique<RmRecord>(tuples_[pos_]);
    }
    const RmRecord *Current() const override { return is_end() ? nullptr : &tuples_[pos_]; }
    Rid &rid() override { return _abstract_rid; }
};
