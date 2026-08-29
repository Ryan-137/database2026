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
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index_helper.h"
#include "index/ix.h"
#include "system/sm.h"
#include "transaction/transaction_manager.h"

class InsertExecutor : public AbstractExecutor {
   private:
    TabMeta *tab_{nullptr};         // 语句 gate 生命周期内稳定的表元数据
    std::vector<Value> values_;     // 需要插入的数据
    RmFileHandle *fh_;              // 表的数据文件句柄
    std::string tab_name_;          // 表名称
    Rid rid_;                       // 插入的位置，由于系统默认插入时不指定位置，因此当前rid_在插入后才赋值
    SmManager *sm_manager_;
    TransactionManager *txn_mgr_;

   public:
    InsertExecutor(SmManager *sm_manager, TransactionManager *txn_mgr, const std::string &tab_name,
                   std::vector<Value> values, Context *context) {
        sm_manager_ = sm_manager;
        txn_mgr_ = txn_mgr;
        tab_ = &sm_manager_->db_.get_table(tab_name);
        values_ = std::move(values);
        tab_name_ = tab_name;
        if (values_.size() != tab_->cols.size()) {
            throw InvalidValueCountError();
        }
        for (size_t i = 0; i < values_.size(); ++i) {
            const auto &col = tab_->cols[i];
            if (col.type != values_[i].type) {
                throw IncompatibleTypeError(coltype2str(col.type), coltype2str(values_[i].type));
            }
            if (values_[i].raw == nullptr) {
                values_[i].init_raw(col.len);
            }
        }
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        context_ = context;
    };

    std::unique_ptr<RmRecord> Next() override {
        // Make record buffer
        const auto &last_col = tab_->cols.back();
        RmRecord rec(last_col.offset + last_col.len);
        for (size_t i = 0; i < values_.size(); i++) {
            const auto &col = tab_->cols[i];
            memcpy(rec.data + col.offset, values_[i].raw->data, col.len);
        }

        struct IndexEntry {
            IxIndexHandle *ih;
            int index_no;
            std::vector<char> key;
            IxInsertLeafTicket insert_ticket;
        };

        std::vector<IndexEntry> index_entries;
        for (size_t index_no = 0; index_no < tab_->indexes.size(); ++index_no) {
            auto &index = tab_->indexes[index_no];
            auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
            auto key = IndexHelper::MakeKey(index, rec);
            index_entries.push_back({ih, static_cast<int>(index_no), std::move(key), {}});
        }

        const bool use_insert_leaf_ticket = IxIndexHandle::InsertLeafTicketEnabled();

        // 回退模式保留原表级串行锁；默认路径依靠一次性 RID 预约实现同表并发。
        std::unique_lock<std::mutex> insert_lock;
        if (!RmFileHandle::ParallelInsertEnabled()) {
            insert_lock = fh_->LockInsertMutex();
        }
        auto slot_reservation = fh_->reserve_insert_slot();
        rid_ = slot_reservation.rid();

        std::vector<UniqueKeyChange> unique_key_changes;
        try {
            auto logical_key = IndexHelper::MakeImplicitLogicalKey(*tab_, rec);
            if (!logical_key.empty()) {
                txn_mgr_->CheckPendingKeyDeleteConflict(fh_->GetFd(), IndexHelper::kImplicitLogicalKeyIndexNo,
                                                        logical_key, context_->txn_);
            }
            for (auto &entry : index_entries) {
                // B+ 点查不持有 unique shard 锁；用 generation 做乐观验证，只有并发
                // owner 发布跨过该窗口时才重探测，稳态仍是一轮点查。
                while (true) {
                    std::uint64_t generation =
                        txn_mgr_->GetUniqueKeyGeneration(fh_->GetFd(), entry.index_no, entry.key);
                    Rid current_owner;
                    std::optional<Rid> owner = entry.ih->get_value(
                                                   entry.key.data(), &current_owner, context_->txn_,
                                                   use_insert_leaf_ticket ? &entry.insert_ticket : nullptr)
                                                   ? std::optional<Rid>(current_owner)
                                                   : std::nullopt;
                    if (txn_mgr_->ReserveUniqueKey(fh_->GetFd(), entry.index_no, entry.key, rid_,
                                                   context_->txn_, owner, generation, &unique_key_changes)) {
                        // B+ 当前树不包含已提交 DELETE/UPDATE 的旧键；旧快照仍能通过
                        // stale registry 看见该逻辑键时，INSERT 不能在同一快照制造重复键。
                        auto stale_owners = txn_mgr_->LookupStaleIndexEqual(
                            fh_->GetFd(), entry.index_no, entry.key, context_->txn_);
                        if (std::any_of(stale_owners.begin(), stale_owners.end(),
                                        [&](const Rid &stale_rid) { return stale_rid != rid_; })) {
                            throw DuplicateKeyError();
                        }
                        break;
                    }
                }
            }

            if (context_->txn_->get_isolation_level() == IsolationLevel::SERIALIZABLE) {
                txn_mgr_->RecordSerializableWrite(fh_->GetFd(), tab_name_, rid_, std::nullopt, rec, tab_->cols,
                                                  context_->txn_);
            }
            txn_mgr_->RecordInsert(fh_, rid_, context_->txn_);
            WriteRecord *write_record = new WriteRecord(WType::INSERT_TUPLE, tab_name_, rid_, rec);
            try {
                context_->txn_->append_write_record(write_record);
            } catch (...) {
                delete write_record;
                throw;
            }
        } catch (StatementConflictRetry &) {
            txn_mgr_->RestoreUniqueKeyChanges(unique_key_changes, context_->txn_);
            txn_mgr_->RestoreTupleStateForStatement(TupleKey{fh_->GetFd(), rid_}, context_->txn_);
            throw;
        } catch (...) {
            txn_mgr_->RestoreUniqueKeyChanges(unique_key_changes, context_->txn_);
            txn_mgr_->RestoreTupleStateForStatement(TupleKey{fh_->GetFd(), rid_}, context_->txn_);
            throw;
        }

        try {
            // After this point a heap DML WAL record exists. Any later failure must abort the whole transaction.
            lsn_t insert_lsn = fh_->insert_record(slot_reservation, rec.data, context_, tab_name_);
            if (insert_lock.owns_lock()) {
                insert_lock.unlock();
            }
            for (auto &entry : index_entries) {
                entry.ih->insert_entry(entry.key.data(), rid_, context_->txn_, insert_lsn, true,
                                       use_insert_leaf_ticket ? &entry.insert_ticket : nullptr);
            }
        } catch (...) {
            throw TransactionAbortException(context_->txn_->get_transaction_id(), AbortReason::WRITE_WRITE_CONFLICT);
        }
        return nullptr;
    }
    Rid &rid() override { return rid_; }
};
