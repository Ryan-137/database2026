/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include <cfloat>
#include <climits>
#include <cstring>
#include <memory>
#include <optional>
#include <set>
#include <shared_mutex>
#include <vector>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "mvcc_index_probe.h"
#include "index/ix.h"
#include "system/sm.h"
#include "transaction/transaction_manager.h"

class IndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;                      // 表名称
    std::string real_tab_name_;
    TabMeta *tab_{nullptr};                     // 语句 gate 生命周期内稳定的表元数据
    std::vector<Condition> conds_;              // 扫描条件
    RmFileHandle *fh_;                          // 表的数据文件句柄
    std::vector<ColMeta> cols_;                 // 需要读取的字段
    size_t len_;                                // 选取出来的一条记录的长度
    std::vector<Condition> fed_conds_;          // 扫描条件，和conds_字段相同

    std::vector<std::string> index_col_names_;  // index scan涉及到的索引包含的字段
    IndexMeta *index_meta_{nullptr};            // 语句 gate 生命周期内稳定的索引元数据
    int index_no_{-1};

    Rid rid_;
    MvccIndexProbe::CandidateRids candidate_rids_;
    size_t candidate_pos_{0};
    bool skip_scan_{false};
    std::unique_ptr<RmScan> fallback_scan_;
    std::unique_ptr<RmRecord> current_tuple_;  // 缓存当前命中记录，供 Next() 复用，避免重复读页
    std::optional<UniqueKeyId> exact_unique_point_key_;
    ScanExecutionMode mode_{ScanExecutionMode::VISIBLE_TUPLE};

    SmManager *sm_manager_;
    TransactionManager *txn_mgr_;

    bool predicate_is_exact() const {
        if (fed_conds_.empty()) {
            return false;
        }
        for (const auto &cond : fed_conds_) {
            if (!cond.is_rhs_val || cond.lhs_col.tab_name != tab_name_) {
                return false;
            }
        }
        return true;
    }

    // 根据列类型填充索引扫描边界的哨兵值，保证范围尽量宽、不漏记录
    void fill_sentinel(char *dest, const ColMeta &col, bool upper) {
        if (col.type == TYPE_INT) {
            int value = upper ? INT_MAX : INT_MIN;
            memcpy(dest, &value, sizeof(int));
        } else if (col.type == TYPE_FLOAT) {
            float value = upper ? FLT_MAX : -FLT_MAX;
            memcpy(dest, &value, sizeof(float));
        } else {
            memset(dest, upper ? 0xFF : 0x00, col.len);
        }
    }

    // 查找当前索引列上的常量条件；等值优先，范围条件用于构造当前列上下界
    void find_column_conds(const ColMeta &index_col, Condition **eq_cond,
                           Condition **lower_cond, Condition **upper_cond) {
        *eq_cond = nullptr;
        *lower_cond = nullptr;
        *upper_cond = nullptr;
        for (auto &cond : fed_conds_) {
            if (!cond.is_rhs_val || cond.lhs_col.tab_name != tab_name_ || cond.lhs_col.col_name != index_col.name) {
                continue;
            }
            if (cond.rhs_val.raw == nullptr) {
                cond.rhs_val.init_raw(index_col.len);
            }
            if (cond.op == OP_EQ) {
                *eq_cond = &cond;
            } else if (cond.op == OP_GT || cond.op == OP_GE) {
                if (*lower_cond == nullptr ||
                    ix_compare(cond.rhs_val.raw->data, (*lower_cond)->rhs_val.raw->data,
                               index_col.type, index_col.len) > 0) {
                    *lower_cond = &cond;
                }
            } else if (cond.op == OP_LT || cond.op == OP_LE) {
                if (*upper_cond == nullptr ||
                    ix_compare(cond.rhs_val.raw->data, (*upper_cond)->rhs_val.raw->data,
                               index_col.type, index_col.len) < 0) {
                    *upper_cond = &cond;
                }
            }
        }
    }

    void fill_full_bounds(std::vector<char> &lower_key, std::vector<char> &upper_key) {
        lower_key.resize(index_meta_->col_tot_len);
        upper_key.resize(index_meta_->col_tot_len);
        int offset = 0;

        for (auto &index_col : index_meta_->cols) {
            fill_sentinel(lower_key.data() + offset, index_col, false);
            fill_sentinel(upper_key.data() + offset, index_col, true);
            offset += index_col.len;
        }
    }

    // 构造完整长度的 [lower_key, upper_key]；不参与前缀的列保持 MIN/MAX 哨兵值
    void build_scan_keys(std::vector<char> &lower_key, std::vector<char> &upper_key) {
        fill_full_bounds(lower_key, upper_key);

        int offset = 0;
        for (auto &index_col : index_meta_->cols) {
            Condition *eq_cond = nullptr;
            Condition *lower_cond = nullptr;
            Condition *upper_cond = nullptr;
            find_column_conds(index_col, &eq_cond, &lower_cond, &upper_cond);

            if (eq_cond != nullptr) {
                memcpy(lower_key.data() + offset, eq_cond->rhs_val.raw->data, index_col.len);
                memcpy(upper_key.data() + offset, eq_cond->rhs_val.raw->data, index_col.len);
                offset += index_col.len;
                continue;
            }

            if (lower_cond != nullptr) {
                memcpy(lower_key.data() + offset, lower_cond->rhs_val.raw->data, index_col.len);
            }
            if (upper_cond != nullptr) {
                memcpy(upper_key.data() + offset, upper_cond->rhs_val.raw->data, index_col.len);
            }
            break;
        }
    }

    // 仅跳过一个前导列；其后的连续等值条件及至多一个范围条件用于收窄每个子区间。
    void build_skip_scan_keys(std::vector<char> &lower_key, std::vector<char> &upper_key,
                              std::vector<char> &full_lower_key, std::vector<char> &full_upper_key) {
        fill_full_bounds(full_lower_key, full_upper_key);
        lower_key = full_lower_key;
        upper_key = full_upper_key;
        int offset = index_meta_->cols.front().len;
        for (size_t i = 1; i < index_meta_->cols.size(); ++i) {
            auto &index_col = index_meta_->cols[i];
            Condition *eq_cond = nullptr;
            Condition *lower_cond = nullptr;
            Condition *upper_cond = nullptr;
            find_column_conds(index_col, &eq_cond, &lower_cond, &upper_cond);
            if (eq_cond != nullptr) {
                memcpy(lower_key.data() + offset, eq_cond->rhs_val.raw->data, index_col.len);
                memcpy(upper_key.data() + offset, eq_cond->rhs_val.raw->data, index_col.len);
                offset += index_col.len;
                continue;
            }
            if (lower_cond != nullptr) {
                memcpy(lower_key.data() + offset, lower_cond->rhs_val.raw->data, index_col.len);
            }
            if (upper_cond != nullptr) {
                memcpy(upper_key.data() + offset, upper_cond->rhs_val.raw->data, index_col.len);
            }
            break;
        }
    }

    void collect_candidates(const std::vector<char> &lower_key, const std::vector<char> &upper_key) {
        candidate_pos_ = 0;
        candidate_rids_ = MvccIndexProbe::CollectCandidates(sm_manager_, txn_mgr_, real_tab_name_, fh_, *index_meta_,
                                                            index_no_, lower_key, upper_key, context_->txn_);
    }

    void collect_skip_scan_candidates(const std::vector<char> &lower_key, const std::vector<char> &upper_key,
                                      const std::vector<char> &full_lower_key,
                                      const std::vector<char> &full_upper_key) {
        static constexpr size_t kMaxSkipPrefixes = 16;
        static constexpr size_t kMaxSkipCandidates = 32;
        candidate_pos_ = 0;
        auto result = MvccIndexProbe::CollectSkipScanCandidates(
            sm_manager_, txn_mgr_, real_tab_name_, fh_, *index_meta_, index_no_, index_meta_->cols.front().len,
            lower_key, upper_key, full_lower_key, full_upper_key, kMaxSkipPrefixes, kMaxSkipCandidates,
            context_->txn_);
        candidate_rids_.clear();
        for (const auto &rid : result.rids) {
            candidate_rids_.push_back(rid);
        }
        if (result.fallback_to_seq_scan) {
            fallback_scan_ = std::make_unique<RmScan>(fh_);
        }
    }

    std::unique_ptr<RmRecord> get_visible_record(const Rid &rid) {
        return txn_mgr_->ReadVisibleTuple(fh_, rid, context_, context_->txn_);
    }

    // 从候选 RID 中推进到第一条满足 MVCC 可见性和完整 where 条件的记录
    void advance_to_next_valid() {
        current_tuple_ = nullptr;
        while (candidate_pos_ < candidate_rids_.size()) {
            rid_ = candidate_rids_[candidate_pos_++];
            auto visible = get_visible_record(rid_);
            if (visible == nullptr) {
                continue;
            }
            if (!eval_conds(cols_, visible.get(), fed_conds_)) {
                continue;
            }
            current_tuple_ = std::move(visible);
            if (exact_unique_point_key_.has_value() && context_->txn_ != nullptr) {
                context_->txn_->MarkExpectedExistingUniquePoint(exact_unique_point_key_.value());
            }
            if (context_->txn_ != nullptr &&
                context_->txn_->get_isolation_level() == IsolationLevel::SERIALIZABLE) {
                txn_mgr_->RecordSerializableTupleRead(fh_->GetFd(), rid_, context_->txn_);
            }
            return;
        }
    }

    void advance_fallback_to_next_valid() {
        current_tuple_ = nullptr;
        while (fallback_scan_ != nullptr && !fallback_scan_->is_end()) {
            rid_ = fallback_scan_->rid();
            auto visible = get_visible_record(rid_);
            if (visible != nullptr && eval_conds(cols_, visible.get(), fed_conds_)) {
                current_tuple_ = std::move(visible);
                if (context_->txn_ != nullptr &&
                    context_->txn_->get_isolation_level() == IsolationLevel::SERIALIZABLE) {
                    txn_mgr_->RecordSerializableTupleRead(fh_->GetFd(), rid_, context_->txn_);
                }
                return;
            }
            fallback_scan_->next();
        }
    }

   public:
    IndexScanExecutor(SmManager *sm_manager, TransactionManager *txn_mgr, std::string tab_name,
                      std::vector<Condition> conds, std::vector<std::string> index_col_names,
                      Context *context, std::string real_tab_name = "", bool skip_scan = false,
                      ScanExecutionMode mode = ScanExecutionMode::VISIBLE_TUPLE) {
        sm_manager_ = sm_manager;
        txn_mgr_ = txn_mgr;
        context_ = context;
        skip_scan_ = skip_scan;
        mode_ = mode;
        tab_name_ = std::move(tab_name);
        real_tab_name_ = real_tab_name.empty() ? tab_name_ : std::move(real_tab_name);
        tab_ = &sm_manager_->db_.get_table(real_tab_name_);
        conds_ = std::move(conds);
        // index_no_ = index_no;
        index_col_names_ = index_col_names;
        auto index_it = tab_->get_index_meta(index_col_names_);
        index_no_ = static_cast<int>(std::distance(tab_->indexes.begin(), index_it));
        index_meta_ = &*index_it;
        fh_ = sm_manager_->fhs_.at(real_tab_name_).get();
        cols_ = tab_->cols;
        for (auto &col : cols_) col.tab_name = tab_name_;
        len_ = cols_.back().offset + cols_.back().len;
        std::map<CompOp, CompOp> swap_op = {
            {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
        };

        for (auto &cond : conds_) {
            if (cond.lhs_col.tab_name != tab_name_) {
                // lhs is on other table, now rhs must be on this table
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == tab_name_);
                // swap lhs and rhs
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_op.at(cond.op);
            }
        }
        fed_conds_ = conds_;
        bind_conds(cols_, &fed_conds_);
    }

    void beginTuple() override {
        if (context_->txn_ != nullptr &&
            context_->txn_->get_isolation_level() == IsolationLevel::SERIALIZABLE) {
            bool exact = predicate_is_exact();
            txn_mgr_->RecordSerializablePredicateRead(fh_->GetFd(), real_tab_name_,
                                                      exact ? fed_conds_ : std::vector<Condition>(), cols_,
                                                      fed_conds_.empty() || !exact,
                                                      !fed_conds_.empty() && !exact, context_->txn_);
        }
        fallback_scan_.reset();
        candidate_rids_.clear();
        exact_unique_point_key_.reset();
        if (skip_scan_) {
            std::vector<char> lower_key;
            std::vector<char> upper_key;
            std::vector<char> full_lower_key;
            std::vector<char> full_upper_key;
            build_skip_scan_keys(lower_key, upper_key, full_lower_key, full_upper_key);
            collect_skip_scan_candidates(lower_key, upper_key, full_lower_key, full_upper_key);
        } else {
            std::vector<char> lower_key;
            std::vector<char> upper_key;
            build_scan_keys(lower_key, upper_key);
            if (lower_key == upper_key) {
                exact_unique_point_key_ =
                    UniqueKeyId{fh_->GetFd(), index_no_, std::string(lower_key.data(), lower_key.size())};
            }
            collect_candidates(lower_key, upper_key);
        }
        if (mode_ == ScanExecutionMode::CANDIDATE_RID) {
            current_tuple_.reset();
            if (fallback_scan_ != nullptr) {
                if (!fallback_scan_->is_end()) {
                    rid_ = fallback_scan_->rid();
                }
            } else if (!candidate_rids_.empty()) {
                rid_ = candidate_rids_[0];
            }
        } else if (fallback_scan_ != nullptr) {
            advance_fallback_to_next_valid();
        } else {
            advance_to_next_valid();
        }
    }

    void nextTuple() override {
        if (mode_ == ScanExecutionMode::CANDIDATE_RID) {
            if (fallback_scan_ != nullptr) {
                fallback_scan_->next();
                if (!fallback_scan_->is_end()) {
                    rid_ = fallback_scan_->rid();
                }
            } else {
                ++candidate_pos_;
                if (candidate_pos_ < candidate_rids_.size()) {
                    rid_ = candidate_rids_[candidate_pos_];
                }
            }
            return;
        }
        if (fallback_scan_ != nullptr) {
            fallback_scan_->next();
            advance_fallback_to_next_valid();
        } else {
            advance_to_next_valid();
        }
    }

    bool is_end() const override {
        if (mode_ == ScanExecutionMode::CANDIDATE_RID) {
            return fallback_scan_ != nullptr ? fallback_scan_->is_end()
                                             : candidate_pos_ >= candidate_rids_.size();
        }
        return current_tuple_ == nullptr;
    }

    std::unique_ptr<RmRecord> Next() override {
        if (mode_ == ScanExecutionMode::CANDIDATE_RID) {
            return nullptr;
        }
        // 返回缓存记录的拷贝：Next() 可能被上层（如 join）对同一行多次调用，需保证可重复读取
        return current_tuple_ ? std::make_unique<RmRecord>(*current_tuple_) : nullptr;
    }

    const RmRecord *Current() const override { return current_tuple_.get(); }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    std::string getType() override { return skip_scan_ ? "IndexSkipScanExecutor" : "IndexScanExecutor"; }

    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }

    Rid &rid() override { return rid_; }
};
