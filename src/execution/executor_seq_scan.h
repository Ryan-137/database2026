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

#include <memory>
#include <shared_mutex>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"
#include "transaction/transaction_manager.h"

class SeqScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;              // 表的名称
    std::string real_tab_name_;
    std::vector<Condition> conds_;      // scan的条件
    RmFileHandle *fh_;                  // 表的数据文件句柄
    std::vector<ColMeta> cols_;         // scan后生成的记录的字段
    size_t len_;                        // scan后生成的每条记录的长度
    std::vector<Condition> fed_conds_;  // 同conds_，两个字段相同

    Rid rid_;
    std::unique_ptr<RecScan> scan_;     // table_iterator
    std::unique_ptr<RmRecord> current_tuple_;  // 缓存当前命中记录，供 Next() 复用，避免重复读页
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

    // 从当前位置开始向后扫描，直到找到第一条满足条件的记录或扫描结束
    void advance_to_next_valid() {
        current_tuple_ = AbstractExecutor::advance_to_next_valid(scan_.get(), &rid_, cols_, fed_conds_,
            [&](const Rid &rid) -> std::unique_ptr<RmRecord> {
                return txn_mgr_->ReadVisibleTuple(fh_, rid, context_, context_->txn_);
            });
        if (current_tuple_ != nullptr) {
            if (context_->txn_ != nullptr &&
                context_->txn_->get_isolation_level() == IsolationLevel::SERIALIZABLE) {
                txn_mgr_->RecordSerializableTupleRead(fh_->GetFd(), rid_, context_->txn_);
            }
        }
    }

   public:
    SeqScanExecutor(SmManager *sm_manager, TransactionManager *txn_mgr, std::string tab_name,
                    std::vector<Condition> conds, Context *context, std::string real_tab_name = "",
                    ScanExecutionMode mode = ScanExecutionMode::VISIBLE_TUPLE) {
        sm_manager_ = sm_manager;
        txn_mgr_ = txn_mgr;
        tab_name_ = std::move(tab_name);
        real_tab_name_ = real_tab_name.empty() ? tab_name_ : std::move(real_tab_name);
        conds_ = std::move(conds);
        TabMeta &tab = sm_manager_->db_.get_table(real_tab_name_);
        fh_ = sm_manager_->fhs_.at(real_tab_name_).get();
        cols_ = tab.cols;
        for (auto &col : cols_) col.tab_name = tab_name_;
        len_ = cols_.back().offset + cols_.back().len;

        context_ = context;
        mode_ = mode;

        fed_conds_ = conds_;
        bind_conds(cols_, &fed_conds_);
    }

    // 上层投影、连接等算子需要通过 tupleLen/cols 获取当前算子的输出格式
    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    std::string getType() override { return "SeqScanExecutor"; }

    void beginTuple() override {
        if (context_->txn_ != nullptr &&
            context_->txn_->get_isolation_level() == IsolationLevel::SERIALIZABLE) {
            bool exact = predicate_is_exact();
            txn_mgr_->RecordSerializablePredicateRead(fh_->GetFd(), real_tab_name_,
                                                      exact ? fed_conds_ : std::vector<Condition>(),
                                                      cols_, fed_conds_.empty() || !exact,
                                                      !fed_conds_.empty() && !exact, context_->txn_);
        }
        // 每次开始执行时新建表扫描器，并定位到第一条有效记录
        scan_ = std::make_unique<RmScan>(fh_);
        if (mode_ == ScanExecutionMode::CANDIDATE_RID) {
            current_tuple_.reset();
            if (!scan_->is_end()) {
                rid_ = scan_->rid();
            }
            return;
        }
        advance_to_next_valid();
    }

    void nextTuple() override {
        // 当前记录已经被消费，先移动到下一条物理记录，再继续跳过不满足条件的记录
        scan_->next();
        if (mode_ == ScanExecutionMode::CANDIDATE_RID) {
            if (!scan_->is_end()) {
                rid_ = scan_->rid();
            }
            return;
        }
        advance_to_next_valid();
    }

    bool is_end() const override {
        return scan_ == nullptr || scan_->is_end();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (mode_ == ScanExecutionMode::CANDIDATE_RID) {
            return nullptr;
        }
        // 返回缓存记录的拷贝：Next() 可能被上层（如 join）对同一行多次调用，需保证可重复读取
        return current_tuple_ ? std::make_unique<RmRecord>(*current_tuple_) : nullptr;
    }

    const RmRecord *Current() const override { return current_tuple_.get(); }

    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }

    Rid &rid() override { return rid_; }
};
