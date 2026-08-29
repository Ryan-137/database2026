#pragma once

#include <algorithm>
#include <cstring>
#include <memory>
#include <shared_mutex>
#include <vector>

#include "executor_abstract.h"
#include "index/ix.h"
#include "mvcc_index_probe.h"
#include "system/sm.h"
#include "transaction/transaction_manager.h"

class IndexNestedLoopJoinExecutor : public AbstractExecutor {
private:
    std::unique_ptr<AbstractExecutor> left_;
    SmManager *sm_manager_;
    TransactionManager *txn_mgr_;
    std::string right_table_;
    std::string right_real_table_;
    std::vector<Condition> right_conds_;
    std::vector<Condition> join_conds_;
    TabCol outer_key_;
    TabCol inner_key_;
    TabMeta right_meta_;
    IndexMeta index_meta_;
    int index_no_{-1};
    RmFileHandle *right_fh_;
    std::vector<ColMeta> right_cols_;
    std::vector<ColMeta> cols_;
    size_t len_ = 0;
    size_t right_len_ = 0;
    MvccIndexProbe::CandidateRids right_candidates_;
    size_t right_candidate_pos_{0};
    Rid current_right_rid_;
    std::unique_ptr<RmRecord> current_right_;
    std::unique_ptr<RmRecord> current_join_;
    bool is_end_ = true;

    Value value_from_raw(const ColMeta &col, const char *raw) const {
        Value value;
        if (col.type == TYPE_INT) {
            value.set_int(*reinterpret_cast<const int *>(raw));
        } else if (col.type == TYPE_FLOAT) {
            value.set_float(*reinterpret_cast<const float *>(raw));
        } else {
            value.set_str(std::string(raw, strnlen(raw, col.len)));
        }
        value.init_raw(col.len);
        return value;
    }

    Condition probe_predicate(const ColMeta &inner_col, const char *probe_raw) const {
        Condition cond;
        cond.lhs_col = inner_key_;
        cond.op = OP_EQ;
        cond.is_rhs_val = true;
        cond.rhs_val = value_from_raw(inner_col, probe_raw);
        return cond;
    }

    void open_probe() {
        const auto outer_col = left_->get_col_offset(outer_key_);
        const auto inner_col = *std::find_if(right_cols_.begin(), right_cols_.end(), [&](const ColMeta &col) {
            return col.name == inner_key_.col_name;
        });
        std::unique_ptr<RmRecord> owned_left;
        const RmRecord *left_record = left_->CurrentOrNext(&owned_left);
        std::vector<char> key(index_meta_.col_tot_len, 0);
        memcpy(key.data(), left_record->data + outer_col.offset, std::min(outer_col.len, inner_col.len));

        if (context_->txn_ != nullptr &&
            context_->txn_->get_isolation_level() == IsolationLevel::SERIALIZABLE) {
            std::vector<Condition> predicate_conds = right_conds_;
            predicate_conds.push_back(probe_predicate(inner_col, left_record->data + outer_col.offset));
            txn_mgr_->RecordSerializablePredicateRead(right_fh_->GetFd(), right_real_table_, predicate_conds,
                                                      right_cols_, false, false, context_->txn_);
        }

        right_candidates_ = MvccIndexProbe::CollectCandidates(sm_manager_, txn_mgr_, right_real_table_, right_fh_,
                                                              index_meta_, index_no_, key, key, context_->txn_);
        right_candidate_pos_ = 0;
    }

    std::unique_ptr<RmRecord> make_join_record(const RmRecord &right_record) {
        std::unique_ptr<RmRecord> owned_left;
        const RmRecord *left_record = left_->CurrentOrNext(&owned_left);
        auto record = std::make_unique<RmRecord>(static_cast<int>(len_));
        memcpy(record->data, left_record->data, left_->tupleLen());
        memcpy(record->data + left_->tupleLen(), right_record.data, right_len_);
        return record;
    }

    void advance_to_next_valid() {
        while (!left_->is_end()) {
            while (right_candidate_pos_ < right_candidates_.size()) {
                Rid candidate_rid = right_candidates_[right_candidate_pos_++];
                auto visible = txn_mgr_->ReadVisibleTuple(right_fh_, candidate_rid, context_, context_->txn_);
                if (visible == nullptr) {
                    continue;
                }
                if (eval_conds(right_cols_, visible.get(), right_conds_)) {
                    auto joined = make_join_record(*visible);
                    if (eval_conds(cols_, joined.get(), join_conds_)) {
                        current_right_rid_ = candidate_rid;
                        current_right_ = std::move(visible);
                        current_join_ = std::move(joined);
                        if (context_->txn_ != nullptr &&
                            context_->txn_->get_isolation_level() == IsolationLevel::SERIALIZABLE) {
                            txn_mgr_->RecordSerializableTupleRead(right_fh_->GetFd(), current_right_rid_,
                                                                  context_->txn_);
                        }
                        is_end_ = false;
                        return;
                    }
                }
            }
            left_->nextTuple();
            if (!left_->is_end()) open_probe();
        }
        current_right_.reset();
        current_join_.reset();
        is_end_ = true;
    }

public:
    IndexNestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, SmManager *sm_manager,
                                TransactionManager *txn_mgr,
                                std::string right_table, std::string right_real_table,
                                std::vector<Condition> right_conds,
                                std::vector<Condition> join_conds, TabCol outer_key, TabCol inner_key,
                                Context *context)
        : left_(std::move(left)), sm_manager_(sm_manager), txn_mgr_(txn_mgr), right_table_(std::move(right_table)),
          right_real_table_(right_real_table.empty() ? right_table_ : std::move(right_real_table)),
          right_conds_(std::move(right_conds)), join_conds_(std::move(join_conds)),
          outer_key_(std::move(outer_key)), inner_key_(std::move(inner_key)) {
        context_ = context;
        right_meta_ = sm_manager_->db_.get_table(right_real_table_);
        right_cols_ = right_meta_.cols;
        for (auto &col : right_cols_) col.tab_name = right_table_;
        right_fh_ = sm_manager_->fhs_.at(right_real_table_).get();
        auto index_it = right_meta_.get_index_meta({inner_key_.col_name});
        index_no_ = static_cast<int>(std::distance(right_meta_.indexes.begin(), index_it));
        index_meta_ = *index_it;
        right_len_ = right_cols_.back().offset + right_cols_.back().len;
        len_ = left_->tupleLen() + right_len_;
        cols_ = left_->cols();
        for (auto col : right_cols_) {
            col.offset += left_->tupleLen();
            cols_.push_back(col);
        }
        bind_conds(right_cols_, &right_conds_);
        bind_conds(cols_, &join_conds_);
    }

    void beginTuple() override {
        left_->beginTuple();
        if (left_->is_end()) {
            is_end_ = true;
            return;
        }
        open_probe();
        advance_to_next_valid();
    }

    void nextTuple() override {
        if (is_end_) return;
        advance_to_next_valid();
    }

    bool is_end() const override { return is_end_; }
    std::unique_ptr<RmRecord> Next() override {
        return is_end_ ? nullptr : std::make_unique<RmRecord>(*current_join_);
    }
    const RmRecord *Current() const override { return current_join_.get(); }
    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    std::string getType() override { return "IndexNestedLoopJoinExecutor"; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
    Rid &rid() override { return _abstract_rid; }
};
