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
#include <cstring>
#include <memory>
#include <optional>
#include <set>
#include <string>
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

class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta *tab_{nullptr};
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::unique_ptr<AbstractExecutor> candidate_source_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;
    TransactionManager *txn_mgr_;
    bool expect_existing_unique_point_{false};
    std::optional<UniqueKeyId> expected_unique_point_key_;

    void bind_update_expr(UpdateExpr *expr) {
        if (expr == nullptr) {
            return;
        }
        if (expr->kind == UpdateExprKind::COLUMN) {
            auto col = get_col(tab_->cols, expr->column);
            expr->bound_offset = col->offset;
            expr->bound_type = col->type;
            expr->bound_len = col->len;
        }
        bind_update_expr(expr->lhs.get());
        bind_update_expr(expr->rhs.get());
    }

    Value value_from_record(const RmRecord &record, const ColMeta &col) const {
        Value value;
        const char *raw = record.data + col.offset;
        if (col.type == TYPE_INT) {
            value.set_int(*reinterpret_cast<const int *>(raw));
        } else if (col.type == TYPE_FLOAT) {
            value.set_float(*reinterpret_cast<const float *>(raw));
        } else if (col.type == TYPE_STRING) {
            value.set_str(std::string(raw, strnlen(raw, col.len)));
        }
        return value;
    }

    Value eval_update_expr(const UpdateExpr &expr, const RmRecord &old_record, const ColMeta &target_col) const {
        switch (expr.kind) {
            case UpdateExprKind::VALUE:
                return expr.value;
            case UpdateExprKind::COLUMN: {
                if (expr.bound_offset >= 0) {
                    ColMeta bound;
                    bound.offset = expr.bound_offset;
                    bound.type = expr.bound_type;
                    bound.len = expr.bound_len;
                    return value_from_record(old_record, bound);
                }
                auto col = get_col(tab_->cols, expr.column);
                return value_from_record(old_record, *col);
            }
            case UpdateExprKind::ADD:
            case UpdateExprKind::SUB: {
                Value lhs = eval_update_expr(*expr.lhs, old_record, target_col);
                Value rhs = eval_update_expr(*expr.rhs, old_record, target_col);
                if ((lhs.type != TYPE_INT && lhs.type != TYPE_FLOAT) ||
                    (rhs.type != TYPE_INT && rhs.type != TYPE_FLOAT)) {
                    throw IncompatibleTypeError("numeric", "STRING");
                }
                bool subtract = expr.kind == UpdateExprKind::SUB;
                if (target_col.type == TYPE_INT) {
                    if (lhs.type != TYPE_INT || rhs.type != TYPE_INT) {
                        throw IncompatibleTypeError(coltype2str(target_col.type), "FLOAT");
                    }
                    Value result;
                    result.set_int(subtract ? lhs.int_val - rhs.int_val : lhs.int_val + rhs.int_val);
                    return result;
                }
                if (target_col.type == TYPE_FLOAT) {
                    float lhs_float = lhs.type == TYPE_FLOAT ? lhs.float_val : static_cast<float>(lhs.int_val);
                    float rhs_float = rhs.type == TYPE_FLOAT ? rhs.float_val : static_cast<float>(rhs.int_val);
                    Value result;
                    result.set_float(subtract ? lhs_float - rhs_float : lhs_float + rhs_float);
                    return result;
                }
                throw IncompatibleTypeError("numeric", coltype2str(target_col.type));
            }
        }
        throw InternalError("Unexpected update expression type");
    }

   public:
    UpdateExecutor(SmManager *sm_manager, TransactionManager *txn_mgr, const std::string &tab_name,
                   std::vector<SetClause> set_clauses, std::vector<Condition> conds,
                   std::unique_ptr<AbstractExecutor> candidate_source,
                   Context *context, bool expect_existing_unique_point = false) {
        sm_manager_ = sm_manager;
        txn_mgr_ = txn_mgr;
        tab_name_ = tab_name;
        set_clauses_ = std::move(set_clauses);
        tab_ = &sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = std::move(conds);
        candidate_source_ = std::move(candidate_source);
        context_ = context;
        bind_conds(tab_->cols, &conds_);
        for (auto &set_clause : set_clauses_) {
            TabCol target = set_clause.lhs;
            if (target.tab_name.empty()) {
                target.tab_name = tab_name_;
            }
            auto col = get_col(tab_->cols, target);
            set_clause.bound_target_offset = col->offset;
            set_clause.bound_target_type = col->type;
            set_clause.bound_target_len = col->len;
            bind_update_expr(&set_clause.rhs);
        }
        expect_existing_unique_point_ = expect_existing_unique_point;
        if (expect_existing_unique_point_) {
            expected_unique_point_key_ = BuildExpectedUniquePointKey(fh_->GetFd(), *tab_, conds_);
        }
    }
    std::unique_ptr<RmRecord> Next() override {
        auto key_to_string = [](const std::vector<char> &key) {
            return std::string(key.data(), key.size());
        };

        // 保存某条记录在一个索引上的 key 变化，用于校验通过后统一更新索引
        struct IndexChange {
            IxIndexHandle *ih;
            int index_no;
            Rid rid;
            std::vector<char> old_key;
            std::vector<char> new_key;
        };

        // update 先生成待修改记录，全部唯一约束校验通过后再真正写表和索引
        struct PendingUpdate {
            Rid rid;
            std::unique_ptr<RmRecord> old_record;
            std::unique_ptr<RmRecord> new_record;
            std::vector<IndexChange> index_changes;
            lsn_t update_lsn{INVALID_LSN};
        };

        std::vector<PendingUpdate> pending_updates;
        pending_updates.reserve(4);
        bool saw_expected_key_visible_candidate = false;
        // 记录本次 update 内部将要生成的新唯一 key，防止多行同时更新成同一个 key
        std::set<std::pair<IxIndexHandle *, std::string>> new_unique_keys;

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
                // 与原先“可见扫描后再执行 DML”的语义一致：一旦本事务确实读到该唯一键，
                // 后续同键语句若因并发提交而找不到记录，必须按 FCW 冲突中止，不能静默漏写。
                context_->txn_->MarkExpectedExistingUniquePoint(expected_unique_point_key_.value());
            }
            if (context_->txn_->get_isolation_level() == IsolationLevel::SERIALIZABLE) {
                txn_mgr_->RecordSerializableTupleRead(fh_->GetFd(), rid, context_->txn_);
            }
            auto new_record = std::make_unique<RmRecord>(*old_record);

            for (auto &set_clause : set_clauses_) {
                ColMeta target_col;
                target_col.offset = set_clause.bound_target_offset;
                target_col.type = set_clause.bound_target_type;
                target_col.len = set_clause.bound_target_len;
                Value rhs = eval_update_expr(set_clause.rhs, *old_record, target_col);
                if (target_col.type == TYPE_FLOAT && rhs.type == TYPE_INT) {
                    rhs.set_float(static_cast<float>(rhs.int_val));
                }
                if (target_col.type != rhs.type) {
                    throw IncompatibleTypeError(coltype2str(target_col.type), coltype2str(rhs.type));
                }
                rhs.init_raw(target_col.len);
                memcpy(new_record->data + target_col.offset, rhs.raw->data, target_col.len);
            }

            PendingUpdate pending;
            pending.rid = rid;
            for (size_t index_no = 0; index_no < tab_->indexes.size(); ++index_no) {
                auto &index = tab_->indexes[index_no];
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
                auto old_key = IndexHelper::MakeKey(index, *old_record);
                auto new_key = IndexHelper::MakeKey(index, *new_record);
                if (old_key == new_key) {
                    continue;
                }

                // 本次批量 update 内部不能生成重复唯一 key；与表中已有 key 的冲突由 insert_entry 检查
                if (!new_unique_keys.emplace(ih, key_to_string(new_key)).second) {
                    throw DuplicateKeyError();
                }

                pending.index_changes.push_back({ih, static_cast<int>(index_no), rid, std::move(old_key),
                                                 std::move(new_key)});
            }

            pending.old_record = std::move(old_record);
            pending.new_record = std::move(new_record);
            pending_updates.push_back(std::move(pending));
        }
        if (expect_existing_unique_point_ && pending_updates.empty() &&
            !saw_expected_key_visible_candidate && expected_unique_point_key_.has_value() &&
            context_->txn_->HasExpectedExistingUniquePoint(expected_unique_point_key_.value()) &&
            !context_->txn_->RemovedUniqueKeyInThisTxn(expected_unique_point_key_.value())) {
            throw TransactionAbortException(context_->txn_->get_transaction_id(), AbortReason::WRITE_WRITE_CONFLICT);
        }

        std::vector<TupleKey> statement_newly_acquired;
        std::vector<TupleKey> registered_tuple_states;
        std::vector<OwnedTupleFlagChange> owned_flag_changes;
        owned_flag_changes.reserve(pending_updates.size());
        std::vector<UniqueKeyChange> unique_key_changes;
        try {
            for (auto &pending : pending_updates) {
                TupleKey tuple_key{fh_->GetFd(), pending.rid};
                auto outcome = txn_mgr_->AcquireWriteAndRecord(fh_, pending.rid, *pending.old_record,
                                                               context_->txn_, TupleWriteKind::UPDATE);
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
            }
            for (auto &pending : pending_updates) {
                for (auto &change : pending.index_changes) {
                    txn_mgr_->MarkUniqueKeyDeleted(fh_->GetFd(), change.index_no, change.old_key, change.rid,
                                                   context_->txn_, &unique_key_changes);
                }
            }
            for (auto &pending : pending_updates) {
                for (auto &change : pending.index_changes) {
                    while (true) {
                        std::uint64_t generation = txn_mgr_->GetUniqueKeyGeneration(
                            fh_->GetFd(), change.index_no, change.new_key);
                        Rid current_owner;
                        std::optional<Rid> owner = change.ih->get_value(change.new_key.data(), &current_owner,
                                                                        context_->txn_)
                                                       ? std::optional<Rid>(current_owner)
                                                       : std::nullopt;
                        if (txn_mgr_->ReserveUniqueKey(fh_->GetFd(), change.index_no, change.new_key, change.rid,
                                                       context_->txn_, owner, generation, &unique_key_changes)) {
                            auto stale_owners = txn_mgr_->LookupStaleIndexEqual(
                                fh_->GetFd(), change.index_no, change.new_key, context_->txn_);
                            if (std::any_of(stale_owners.begin(), stale_owners.end(),
                                            [&](const Rid &stale_rid) { return stale_rid != change.rid; })) {
                                throw DuplicateKeyError();
                            }
                            break;
                        }
                    }
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
                for (auto &pending : pending_updates) {
                    txn_mgr_->RecordSerializableWrite(fh_->GetFd(), tab_name_, pending.rid, *pending.old_record,
                                                      *pending.new_record, tab_->cols, context_->txn_);
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

        std::vector<std::unique_ptr<WriteRecord>> write_records;
        write_records.reserve(pending_updates.size());
        for (auto &pending : pending_updates) {
            write_records.push_back(std::make_unique<WriteRecord>(WType::UPDATE_TUPLE, tab_name_, pending.rid,
                                                                  *pending.old_record, *pending.new_record));
        }

        std::vector<PendingUpdate *> updated_records;
        std::vector<WriteRecord *> appended_records;
        updated_records.reserve(pending_updates.size());
        appended_records.reserve(pending_updates.size());
        try {
            for (size_t i = 0; i < pending_updates.size(); ++i) {
                WriteRecord *write_record = write_records[i].get();
                context_->txn_->append_write_record(write_record);
                write_records[i].release();
                appended_records.push_back(write_record);
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

        std::vector<IndexChange *> deleted_indexes;
        std::vector<IndexChange *> inserted_indexes;
        try {
            // DML WAL must exist before any B+ tree page is changed.
            for (auto &pending : pending_updates) {
                pending.update_lsn = fh_->log_update_record(pending.rid, *pending.old_record, *pending.new_record,
                                                            context_, tab_name_);
            }
            for (auto &pending : pending_updates) {
                for (auto &change : pending.index_changes) {
                    // 先发布旧键候选，再从当前 B+ 树删除旧键。否则读线程可能恰好落在
                    // “旧键已删除、stale key 尚未登记”的窗口，漏掉快照可见记录。
                    txn_mgr_->AddStaleIndexEntry(fh_->GetFd(), change.index_no, change.old_key, change.rid,
                                                 context_->txn_);
                    if (!change.ih->delete_entry(change.old_key.data(), context_->txn_, pending.update_lsn)) {
                        throw IndexEntryNotFoundError();
                    }
                    deleted_indexes.push_back(&change);
                }
            }
            for (auto &pending : pending_updates) {
                for (auto &change : pending.index_changes) {
                    change.ih->insert_entry(change.new_key.data(), change.rid, context_->txn_, pending.update_lsn);
                    inserted_indexes.push_back(&change);
                }
            }
            for (auto &pending : pending_updates) {
                fh_->update_record(pending.rid, pending.new_record->data, nullptr, "", nullptr, pending.update_lsn);
                updated_records.push_back(&pending);
            }
        } catch (...) {
            throw TransactionAbortException(context_->txn_->get_transaction_id(), AbortReason::WRITE_WRITE_CONFLICT);
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
