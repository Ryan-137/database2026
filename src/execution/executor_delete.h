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
#include <algorithm>
#include <memory>
#include <optional>
#include <vector>

#include "execution_defs.h"
#include "execution_manager.h"
#include "expected_unique_point.h"
#include "executor_abstract.h"
#include "index_helper.h"
#include "index/ix.h"
#include "system/sm.h"
#include "transaction/conflict_waiter.h"
#include "transaction/transaction_manager.h"

class DeleteExecutor : public AbstractExecutor {
   private:
    TabMeta *tab_{nullptr};         // 语句 gate 生命周期内稳定的表元数据
    std::vector<Condition> conds_;  // delete的条件
    RmFileHandle *fh_;              // 表的数据文件句柄
    std::unique_ptr<AbstractExecutor> candidate_source_;
    std::string tab_name_;          // 表名称
    SmManager *sm_manager_;
    TransactionManager *txn_mgr_;
    bool expect_existing_unique_point_{false};
    std::optional<UniqueKeyId> expected_unique_point_key_;

   public:
    DeleteExecutor(SmManager *sm_manager, TransactionManager *txn_mgr, const std::string &tab_name,
                   std::vector<Condition> conds, std::unique_ptr<AbstractExecutor> candidate_source, Context *context,
                   bool expect_existing_unique_point = false) {
        sm_manager_ = sm_manager;
        txn_mgr_ = txn_mgr;
        tab_name_ = tab_name;
        tab_ = &sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = std::move(conds);
        candidate_source_ = std::move(candidate_source);
        context_ = context;
        bind_conds(tab_->cols, &conds_);
        expect_existing_unique_point_ = expect_existing_unique_point;
        if (expect_existing_unique_point_) {
            expected_unique_point_key_ = BuildExpectedUniquePointKey(fh_->GetFd(), *tab_, conds_);
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        struct IndexEntry {
            IxIndexHandle *ih;
            int index_no;
            Rid rid;
            std::vector<char> key;
        };

        struct PendingDelete {
            Rid rid;
            std::unique_ptr<RmRecord> old_record;
            std::vector<IndexEntry> index_entries;
        };

        std::vector<PendingDelete> pending_deletes;
        pending_deletes.reserve(4);
        bool saw_expected_key_visible_candidate = false;
        for (candidate_source_->beginTuple(); !candidate_source_->is_end(); candidate_source_->nextTuple()) {
            Rid rid = candidate_source_->rid();
            auto old_record = txn_mgr_->ReadVisibleTuple(fh_, rid, context_, context_->txn_);
            if (old_record == nullptr) {
                continue;
            }
            if (expected_unique_point_key_.has_value() &&
                RecordMatchesExpectedUniquePoint(*tab_, *old_record, expected_unique_point_key_.value())) {
                saw_expected_key_visible_candidate = true;
            }
            if (!eval_conds(tab_->cols, old_record.get(), conds_)) {
                continue;
            }
            if (expected_unique_point_key_.has_value()) {
                // 候选 RID 模式跳过了旧的可见扫描层，因此在最终谓词命中处补齐唯一点读标记。
                context_->txn_->MarkExpectedExistingUniquePoint(expected_unique_point_key_.value());
            }
            if (context_->txn_->get_isolation_level() == IsolationLevel::SERIALIZABLE) {
                txn_mgr_->RecordSerializableTupleRead(fh_->GetFd(), rid, context_->txn_);
            }

            PendingDelete pending;
            pending.rid = rid;
            for (size_t index_no = 0; index_no < tab_->indexes.size(); ++index_no) {
                auto &index = tab_->indexes[index_no];
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
                auto key = IndexHelper::MakeKey(index, *old_record);
                pending.index_entries.push_back({ih, static_cast<int>(index_no), rid, std::move(key)});
            }
            pending.old_record = std::move(old_record);
            pending_deletes.push_back(std::move(pending));
        }
        if (expect_existing_unique_point_ && pending_deletes.empty() &&
            !saw_expected_key_visible_candidate && expected_unique_point_key_.has_value() &&
            context_->txn_->HasExpectedExistingUniquePoint(expected_unique_point_key_.value()) &&
            !context_->txn_->RemovedUniqueKeyInThisTxn(expected_unique_point_key_.value())) {
            throw TransactionAbortException(context_->txn_->get_transaction_id(), AbortReason::WRITE_WRITE_CONFLICT);
        }

        std::vector<std::unique_ptr<WriteRecord>> write_records;
        write_records.reserve(pending_deletes.size());
        for (auto &pending : pending_deletes) {
            write_records.push_back(
                std::make_unique<WriteRecord>(WType::DELETE_TUPLE, tab_name_, pending.rid, *pending.old_record));
        }

        std::vector<TupleKey> statement_newly_acquired;
        std::vector<TupleKey> registered_tuple_states;
        std::vector<OwnedTupleFlagChange> owned_flag_changes;
        owned_flag_changes.reserve(pending_deletes.size());
        std::vector<UniqueKeyChange> unique_key_changes;
        try {
            for (auto &pending : pending_deletes) {
                TupleKey tuple_key{fh_->GetFd(), pending.rid};
                auto outcome = txn_mgr_->AcquireWriteAndRecord(fh_, pending.rid, *pending.old_record,
                                                               context_->txn_, TupleWriteKind::DELETE);
                if (outcome.result != WriteCheckResult::OK) {
                    if (outcome.result == WriteCheckResult::CONFLICT_WITH_ACTIVE_WRITER) {
                        throw StatementConflictRetry(outcome.owner);
                    }
                    GlobalConflictRetryStats().RecordTrueFcwAbort();
                    throw TransactionAbortException(context_->txn_->get_transaction_id(),
                                                    AbortReason::WRITE_WRITE_CONFLICT);
                }
                if (outcome.newly_acquired) {
                    statement_newly_acquired.push_back(tuple_key);
                }
                if (outcome.newly_recorded_tuple_state) {
                    registered_tuple_states.push_back(tuple_key);
                }
                if (outcome.changed_owned_delete_flag) {
                    owned_flag_changes.push_back({tuple_key, outcome.previous_owned_delete_flag});
                }
                auto logical_key = IndexHelper::MakeImplicitLogicalKey(*tab_, *pending.old_record);
                if (!logical_key.empty()) {
                    txn_mgr_->MarkUniqueKeyDeleted(fh_->GetFd(), IndexHelper::kImplicitLogicalKeyIndexNo,
                                                   logical_key, pending.rid, context_->txn_, &unique_key_changes);
                }
                for (auto &entry : pending.index_entries) {
                    txn_mgr_->MarkUniqueKeyDeleted(fh_->GetFd(), entry.index_no, entry.key, entry.rid,
                                                   context_->txn_, &unique_key_changes);
                }
            }
        } catch (...) {
            txn_mgr_->RestoreUniqueKeyChanges(unique_key_changes, context_->txn_);
            txn_mgr_->RestoreOwnedTupleFlags(owned_flag_changes, context_->txn_);
            for (auto it = registered_tuple_states.rbegin(); it != registered_tuple_states.rend(); ++it) {
                txn_mgr_->RestoreTupleStateForStatement(*it, context_->txn_);
            }
            txn_mgr_->ReleaseWriteOwners(statement_newly_acquired, context_->txn_);
            throw;
        }

        try {
            if (context_->txn_->get_isolation_level() == IsolationLevel::SERIALIZABLE) {
                for (auto &pending : pending_deletes) {
                    txn_mgr_->RecordSerializableWrite(fh_->GetFd(), tab_name_, pending.rid, *pending.old_record,
                                                      std::nullopt, tab_->cols, context_->txn_);
                }
            }
        } catch (...) {
            txn_mgr_->RestoreUniqueKeyChanges(unique_key_changes, context_->txn_);
            txn_mgr_->RestoreOwnedTupleFlags(owned_flag_changes, context_->txn_);
            for (auto it = registered_tuple_states.rbegin(); it != registered_tuple_states.rend(); ++it) {
                txn_mgr_->RestoreTupleStateForStatement(*it, context_->txn_);
            }
            txn_mgr_->ReleaseWriteOwners(statement_newly_acquired, context_->txn_);
            throw;
        }

        std::vector<PendingDelete *> deleted_records;
        std::vector<WriteRecord *> appended_records;
        deleted_records.reserve(pending_deletes.size());
        appended_records.reserve(pending_deletes.size());
        try {
            // Q9 DELETE 只做逻辑删除，heap slot 保留给旧快照事务沿统一可见性 helper 读取。
            for (size_t i = 0; i < pending_deletes.size(); ++i) {
                auto &pending = pending_deletes[i];
                WriteRecord *write_record = write_records[i].get();
                context_->txn_->append_write_record(write_record);
                write_records[i].release();
                appended_records.push_back(write_record);
                deleted_records.push_back(&pending);
            }
        } catch (...) {
            txn_mgr_->RestoreUniqueKeyChanges(unique_key_changes, context_->txn_);
            txn_mgr_->RestoreOwnedTupleFlags(owned_flag_changes, context_->txn_);
            for (auto it = registered_tuple_states.rbegin(); it != registered_tuple_states.rend(); ++it) {
                txn_mgr_->RestoreTupleStateForStatement(*it, context_->txn_);
            }
            txn_mgr_->ReleaseWriteOwners(statement_newly_acquired, context_->txn_);

            auto write_set = context_->txn_->get_write_set();
            for (auto it = appended_records.rbegin(); it != appended_records.rend(); ++it) {
                auto pos = std::find(write_set->begin(), write_set->end(), *it);
                if (pos != write_set->end()) {
                    write_set->erase(pos);
                }
                delete *it;
            }
            throw;
        }

        try {
            // Keep the physical index entry until commit. Older snapshots need
            // it to find the tuple, and an abort then has no index structure to
            // reconstruct. The commit path removes it after the COMMIT WAL is
            // durable; recovery performs the same deletion from the DELETE WAL.
            for (auto &pending : pending_deletes) {
                fh_->log_delete_record(pending.rid, *pending.old_record, context_, tab_name_);
                for (auto &entry : pending.index_entries) {
                    txn_mgr_->AddStaleIndexEntry(fh_->GetFd(), entry.index_no, entry.key, entry.rid, context_->txn_);
                }
            }
        } catch (...) {
            throw TransactionAbortException(context_->txn_->get_transaction_id(), AbortReason::WRITE_WRITE_CONFLICT);
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
