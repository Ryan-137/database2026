/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction_manager.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>

#include "execution/index_helper.h"
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"
#include "transaction/conflict_waiter.h"

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

namespace {

int CompareSerializableValue(const char *lhs, const char *rhs, ColType type, int len) {
    if (type == TYPE_INT) {
        int lhs_val = *reinterpret_cast<const int *>(lhs);
        int rhs_val = *reinterpret_cast<const int *>(rhs);
        return (lhs_val > rhs_val) - (lhs_val < rhs_val);
    }
    if (type == TYPE_FLOAT) {
        float lhs_val = *reinterpret_cast<const float *>(lhs);
        float rhs_val = *reinterpret_cast<const float *>(rhs);
        return (lhs_val > rhs_val) - (lhs_val < rhs_val);
    }
    return memcmp(lhs, rhs, len);
}

bool CheckSerializableOp(int cmp, CompOp op) {
    switch (op) {
        case OP_EQ: return cmp == 0;
        case OP_NE: return cmp != 0;
        case OP_LT: return cmp < 0;
        case OP_GT: return cmp > 0;
        case OP_LE: return cmp <= 0;
        case OP_GE: return cmp >= 0;
    }
    return false;
}

timestamp_t SerializableIntervalEnd(timestamp_t finish_ts) {
    return finish_ts == INVALID_TS ? std::numeric_limits<timestamp_t>::max() : finish_ts;
}

bool SerializableIntervalsOverlap(timestamp_t lhs_start, timestamp_t lhs_finish,
                                  timestamp_t rhs_start, timestamp_t rhs_finish) {
    // 提交序区间采用 [start, finish)。begin 取得已发布 commit N 的快照时，
    // 与 finish_ts=N 的事务不再并发；严格小于可避免把前后相继事务误判为重叠。
    return lhs_start < SerializableIntervalEnd(rhs_finish) && rhs_start < SerializableIntervalEnd(lhs_finish);
}

bool SameTupleKey(const std::optional<TupleKey> &lhs, const TupleKey &rhs) {
    return lhs.has_value() && lhs.value() == rhs;
}

}  // namespace

TransactionManager::~TransactionManager() {
    TryDrainRetiredTransactions();
    PrintTupleStateStats();
}

TransactionManager::VersionReadGuard::VersionReadGuard(TransactionManager *manager) : manager_(manager) {
    if (manager_ == nullptr) {
        return;
    }
    slot_ = manager_->GetVersionReaderSlot();
    if (slot_->depth++ == 0) {
        slot_->epoch.store(manager_->version_gc_epoch_.load(std::memory_order_acquire), std::memory_order_seq_cst);
    }
    armed_ = true;
}

TransactionManager::VersionReadGuard::~VersionReadGuard() {
    if (!armed_ || manager_ == nullptr || slot_ == nullptr) {
        return;
    }
    if (--slot_->depth == 0) {
        // 与 retire 线程的 pending 发布构成 SC store-load 握手，避免退出读者与回收者互相错过。
        slot_->epoch.store(0, std::memory_order_seq_cst);
        if (manager_->pending_delete_txn_count_.load(std::memory_order_seq_cst) != 0) {
            manager_->TryDrainRetiredTransactions();
        }
    }
}

TransactionManager::VersionReaderSlot *TransactionManager::GetVersionReaderSlot() {
    static thread_local std::unordered_map<TransactionManager *, VersionReaderSlot *> slots;
    auto it = slots.find(this);
    if (it != slots.end()) {
        return it->second;
    }
    std::lock_guard<std::mutex> lock(version_reader_slots_mutex_);
    version_reader_slots_.push_back(std::make_unique<VersionReaderSlot>());
    auto *slot = version_reader_slots_.back().get();
    slots[this] = slot;
    return slot;
}

void TransactionManager::TryDrainRetiredTransactions() {
    if (pending_delete_txn_count_.load(std::memory_order_seq_cst) == 0) {
        return;
    }
    // Epoch-based version GC: a retired Transaction can be deleted once every
    // active version reader started after its retire epoch. A long reader only
    // keeps entries retired at or after its own epoch; older entries still drain.
    std::uint64_t min_active_epoch = std::numeric_limits<std::uint64_t>::max();
    bool has_active_reader = false;
    {
        std::lock_guard<std::mutex> slots_lock(version_reader_slots_mutex_);
        for (auto &slot : version_reader_slots_) {
            std::uint64_t epoch = slot->epoch.load(std::memory_order_seq_cst);
            if (epoch == 0) {
                continue;
            }
            has_active_reader = true;
            min_active_epoch = std::min(min_active_epoch, epoch);
        }
    }

    std::vector<RetiredTransaction> to_delete;
    {
        std::lock_guard<std::mutex> retired_lock(retired_txns_mutex_);
        size_t kept = 0;
        for (size_t i = 0; i < pending_delete_txns_.size(); ++i) {
            auto &entry = pending_delete_txns_[i];
            if (!has_active_reader || entry.retire_epoch < min_active_epoch) {
                to_delete.push_back(entry);
            } else {
                pending_delete_txns_[kept++] = entry;
            }
        }
        pending_delete_txns_.resize(kept);
        pending_delete_txn_count_.store(pending_delete_txns_.size(), std::memory_order_seq_cst);
    }
    if (!to_delete.empty()) {
        // VersionReadGuard 不只保护对象 delete，也保护 UndoLink -> txn_map lookup。
        // 因此必须等 epoch 安全后才同时摘除 map；提前 erase 会让尚在 guard 内的
        // 读者 FindTransaction 失败并把本应可见的旧版本误判为空。
        std::unique_lock<std::shared_mutex> map_lock(txn_map_mutex_);
        for (const auto &entry : to_delete) {
            auto it = txn_map.find(entry.txn_id);
            if (it != txn_map.end() && it->second == entry.txn) {
                txn_map.erase(it);
            }
        }
    }
    for (const auto &entry : to_delete) {
        delete entry.txn;
    }
}

/**
 * @description: 事务的开始方法
 * @return {Transaction*} 开始事务的指针
 * @param {Transaction*} txn 事务指针，空指针代表需要创建新事务，否则开始已有事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
Transaction * TransactionManager::begin(Transaction* txn, LogManager* log_manager,
                                        IsolationLevel isolation_level) {
    bool counted_active_txn = false;
    bool created_transaction = false;
    bool registered_running_snapshot = false;
    {
        std::unique_lock<std::mutex> checkpoint_lock(checkpoint_mutex_);
        checkpoint_cv_.wait(checkpoint_lock, [&] { return !checkpoint_in_progress_; });
        active_txn_count_++;
        counted_active_txn = true;
    }

    try {
        // 事务指针为空时分配新的事务对象；非空时复用调用方传入的事务对象
        if (txn == nullptr) {
            txn = new Transaction(next_txn_id_++, isolation_level);
            created_transaction = true;
        } else {
            txn->set_isolation_level(isolation_level);
        }

        // 事务进入增长阶段，后续执行器可以继续申请所需锁
        txn->set_state(TransactionState::GROWING);
        {
            // 快照只能取“WAL durable 且内存发布完整”的连续提交前缀。与 AddTxn 在同一
            // publication 临界区完成，避免取到旧快照后尚未登记就被 commit GC 回收 undo。
            std::lock_guard<std::mutex> publication_lock(commit_publication_mutex_);
            timestamp_t start_ts = last_commit_ts_.load(std::memory_order_acquire);
            txn->set_start_ts(start_ts);
            txn->set_read_ts(start_ts);
            running_txns_.AddTxn(start_ts);
            registered_running_snapshot = true;

            // SERIALIZABLE 区间必须与快照登记原子发布。否则并发提交可能先越过
            // publication 点并完成 SSI GC，删掉本事务快照之后仍不可见的 writer
            // intent，导致随后谓词读漏建 rw 反依赖。
            RegisterSerializableBegin(txn);
        }
        txn->set_commit_ts(INVALID_TS);

        if (log_manager != nullptr) {
            BeginLogRecord log_record(txn->get_transaction_id());
            log_record.prev_lsn_ = txn->get_prev_lsn();
            lsn_t lsn = log_manager->add_log_to_buffer(&log_record);
            txn->set_prev_lsn(lsn);
        }

        // 将事务登记到全局事务表，便于同一客户端后续通过事务ID找回事务对象
        std::unique_lock<std::shared_mutex> lock(txn_map_mutex_);
        txn_map[txn->get_transaction_id()] = txn;
        counted_active_txn = false;
        return txn;
    } catch (...) {
        if (registered_running_snapshot && txn != nullptr) {
            running_txns_.RemoveTxn(txn->get_start_ts());
        }
        if (txn != nullptr) {
            try {
                CleanupSerializableAbort(txn->get_transaction_id());
            } catch (...) {
            }
            std::unique_lock<std::shared_mutex> lock(txn_map_mutex_);
            txn_map.erase(txn->get_transaction_id());
        }
        if (counted_active_txn) {
            FinishActiveTransactionForCheckpoint();
        }
        if (created_transaction) {
            delete txn;
        }
        throw;
    }
}

/**
 * @description: 事务的提交方法
 * @param {Transaction*} txn 需要提交的事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    if (txn == nullptr || txn->get_state() == TransactionState::COMMITTED ||
        txn->get_state() == TransactionState::ABORTED) {
        return;
    }

    // 第一阶段只做可能分配内存/抛异常的准备。只要 COMMIT WAL 尚未持久化，
    // 任一步失败都仍可由调用方安全 abort，绝不形成“磁盘已提交、内存又回滚”。
    auto tuple_states_snapshot = txn->GetTupleWriteStatesSnapshot();
    TupleStateCommitGroups tuple_state_groups;
    for (size_t i = 0; i < tuple_states_snapshot.size(); ++i) {
        tuple_state_groups[TupleStateShardIndex(tuple_states_snapshot[i].first)].push_back(i);
    }
    std::vector<TupleKey> tuple_state_gc_keys;
    tuple_state_gc_keys.reserve(tuple_states_snapshot.size());
    for (const auto &group : tuple_state_groups) {
        for (size_t entry_idx : group) {
            tuple_state_gc_keys.push_back(tuple_states_snapshot[entry_idx].first);
        }
    }
    txn->SetTupleStateGcKeys(std::move(tuple_state_gc_keys));

    auto unique_key_states = txn->GetUniqueKeyWriteStatesSnapshot();
    UniqueKeyCommitGroups unique_key_groups;
    for (size_t i = 0; i < unique_key_states.size(); ++i) {
        unique_key_groups[UniqueKeyShardIndex(unique_key_states[i].key)].push_back(i);
    }
    PrepareUniqueKeyCommit(txn, unique_key_states, unique_key_groups);

    auto stale_index_entries = txn->GetStaleIndexEntriesSnapshot();
    StaleIndexCommitGroups stale_index_groups;
    for (size_t i = 0; i < stale_index_entries.size(); ++i) {
        stale_index_groups[StaleIndexBucketIndex(stale_index_entries[i].key.fd,
                                                 stale_index_entries[i].key.index_no)].push_back(i);
    }
    // checkpoint 增量删除集在提交点前预登记。若随后提交失败，abort 会恢复 tuple state，
    // VacuumCommittedDeletes 复核状态后只会忽略这条保守的多余记录。
    std::vector<TupleKey> pending_delete_keys;
    pending_delete_keys.reserve(tuple_states_snapshot.size());
    for (size_t shard_idx = 0; shard_idx < kTupleStateShardCount; ++shard_idx) {
        if (tuple_state_groups[shard_idx].empty()) {
            continue;
        }
        auto &shard = tuple_state_shards_[shard_idx];
        std::shared_lock<std::shared_mutex> lock(shard.mutex);
        for (size_t entry_idx : tuple_state_groups[shard_idx]) {
            const auto &key = tuple_states_snapshot[entry_idx].first;
            auto state_it = shard.states.find(key);
            if (state_it != shard.states.end() && state_it->second.owner == txn->get_transaction_id() &&
                state_it->second.is_deleted) {
                pending_delete_keys.push_back(key);
            }
        }
    }
    if (!pending_delete_keys.empty()) {
        std::lock_guard<std::mutex> vacuum_lock(vacuum_mutex_);
        if (vacuum_tracking_active_) {
            pending_committed_deletes_.reserve(pending_committed_deletes_.size() + pending_delete_keys.size());
            for (const auto &key : pending_delete_keys) {
                pending_committed_deletes_.insert(key);
            }
        }
    }

    std::unique_lock<std::mutex> serializable_commit_lock;
    if (txn->get_isolation_level() == IsolationLevel::SERIALIZABLE) {
        serializable_commit_lock = std::unique_lock<std::mutex>(serializable_commit_mutex_);
    }

    timestamp_t commit_ts = ReserveCommitTimestamp();
    txn->set_commit_ts(commit_ts);
    std::unique_lock<std::mutex> serializable_graph_lock;
    try {
        serializable_graph_lock = PrepareSerializableCommit(txn, commit_ts);
    } catch (...) {
        CancelCommitTimestamp(commit_ts);
        txn->set_commit_ts(INVALID_TS);
        throw;
    }

    // 从这里起提交决定不可撤销。即使 WAL append/flush 抛异常，其持久化边界也可能
    // 已经跨过 COMMIT；因此统一 fail-stop，禁止返回上层再追加 ABORT。
    txn->set_state(TransactionState::SHRINKING);
    lsn_t commit_lsn = INVALID_LSN;
    try {
        if (log_manager != nullptr) {
            CommitLogRecord log_record(txn->get_transaction_id(), commit_ts);
            log_record.prev_lsn_ = txn->get_prev_lsn();
            commit_lsn = log_manager->add_log_to_buffer(&log_record);
            txn->set_prev_lsn(commit_lsn);
            if (commit_lsn == INVALID_LSN) {
                log_manager->flush_log_to_disk();
            } else {
                log_manager->FlushUpTo(commit_lsn);
            }
        }
    } catch (...) {
        std::terminate();
    }

    // 第二阶段的入口就是不可逆提交点。状态必须立即切为 COMMITTED；此后即使发生
    // 不可预期的内部异常，也只能 fail-stop 交给 WAL recovery，绝不能追加 ABORT 回滚。
    txn->set_state(TransactionState::COMMITTED);
    auto write_set = txn->get_write_set();
    try {
        if (serializable_graph_lock.owns_lock()) {
            // SSI 图从 pre-durable 校验到这里始终冻结；COMMIT 已持久化后只做
            // 不抛分配的状态发布，再允许其它 SER 语句建立新边。
            MarkSerializableCommitLocked(txn);
            serializable_graph_lock.unlock();
            serializable_commit_lock.unlock();
        }
        // 按 commit_ts 有序发布 heap/index/tuple/unique/stale。实测把整段提前并行会
        // 放大页锁与缓存争用并恶化 P95，因此只允许 WAL flush 并行，状态发布仍保持
        // 单一顺序；begin 也不会取得半发布快照。
        auto publication_lock = AwaitCommitPublicationTurn(commit_ts);

        // DELETE 在事务活跃期保持索引项；durable 后再删除当前键，旧快照仍可
        // 通过 stale registry 找到 RID，abort 路径则无需重建索引。
        for (auto *write_record : *write_set) {
            if (write_record->GetWriteType() == WType::DELETE_TUPLE) {
                IndexHelper::DeleteAll(sm_manager_, write_record->GetTableName(),
                                       write_record->GetOldRecord(), txn);
            }
        }

        for (size_t shard_idx = 0; shard_idx < kTupleStateShardCount; ++shard_idx) {
            if (tuple_state_groups[shard_idx].empty()) {
                continue;
            }
            auto &shard = tuple_state_shards_[shard_idx];
            std::unique_lock<std::shared_mutex> lock(shard.mutex);
            for (size_t entry_idx : tuple_state_groups[shard_idx]) {
                const auto &key = tuple_states_snapshot[entry_idx].first;
                auto state_it = shard.states.find(key);
                if (state_it != shard.states.end() && state_it->second.owner == txn->get_transaction_id()) {
                    state_it->second.owner = INVALID_TXN_ID;
                    state_it->second.commit_ts = commit_ts;
                    shard.publication_epoch.fetch_add(1, std::memory_order_release);
                }
            }
        }
        txn->ClearTupleWriteStates();
        CommitUniqueKeyEntries(txn, unique_key_states, unique_key_groups);
        CommitStaleIndexEntries(txn, commit_ts, stale_index_entries, stale_index_groups);
        // tuple owner、唯一键 reservation 与 stale 可见性全部发布后再唤醒。若提前
        // 通知，被唤醒事务会再次撞上 pending key，且没有第二次通知只能等轮询超时。
        GlobalConflictWaiter().NotifyRelease();

        for (auto *write_record : *write_set) {
            delete write_record;
        }
        write_set->clear();

        auto lock_set = txn->get_lock_set();
        for (const auto &lock_data_id : *lock_set) {
            lock_manager_->unlock(txn, lock_data_id);
        }
        lock_set->clear();

        // 全部状态完成后原子推进可见提交前缀。
        CompleteCommitPublication(commit_ts, publication_lock);
        running_txns_.UpdateCommitTs(commit_ts);
        running_txns_.RemoveTxn(txn->get_start_ts());
        FinishActiveTransactionForCheckpoint();
    } catch (...) {
        // 已有 durable COMMIT 时继续服务会暴露半发布内存状态；立即 fail-stop，重启按 WAL
        // 重做提交事务，是唯一不会把已提交事务反向回滚的安全结局。
        std::terminate();
    }

    try {
        GarbageCollectStaleIndex();
    } catch (...) {
        std::terminate();
    }
    try {
        GarbageCollectSerializableMetadata();
    } catch (...) {
        // GC 失败只保留更多保守元数据，不改变已提交事务的可见性。
    }
    try {
        MaybeCompactUniqueKeyRegistry();
    } catch (...) {
        // 仅回收空 map 的峰值 bucket；失败时保留容量，不影响唯一性和事务结局。
    }
    try {
        // 已提交事务的 undo 版本可能仍被更旧快照需要，按 watermark 延迟回收。
        RetireTransaction(txn, commit_ts);
    } catch (...) {
        // Retire 可能已从 txn_map 摘除但尚未发布到 pending；继续服务会让旧快照
        // 找不到 undo owner，因此只能 fail-stop 并由 WAL 重建一致状态。
        std::terminate();
    }
}

/**
 * @description: 事务的终止（回滚）方法
 * @param {Transaction *} txn 需要回滚的事务
 * @param {LogManager} *log_manager 日志管理器指针
 */
void TransactionManager::abort(Transaction * txn, LogManager *log_manager) {
    if (txn == nullptr || txn->get_state() == TransactionState::ABORTED) {
        return;
    }
    if (txn->get_state() == TransactionState::COMMITTED) {
        return;
    }
    if (txn->get_state() == TransactionState::SHRINKING) {
        // SHRINKING 表示提交决定已经不可撤销；此时 abort 会制造 COMMIT+ABORT 双结局。
        std::terminate();
    }
    txn->set_state(TransactionState::ABORTED);
    try {
    if (log_manager != nullptr) {
        AbortLogRecord log_record(txn->get_transaction_id());
        log_record.prev_lsn_ = txn->get_prev_lsn();
        lsn_t abort_lsn = log_manager->add_log_to_buffer(&log_record);
        txn->set_prev_lsn(abort_lsn);
        log_manager->FlushUpTo(abort_lsn);
    }
    CleanupSerializableAbort(txn->get_transaction_id());
    auto tuple_states_snapshot = txn->GetTupleWriteStatesSnapshot();

    auto write_set = txn->get_write_set();
    while (!write_set->empty()) {
        // 回滚必须按写入的反方向执行，才能恢复到事务开始前的状态
        WriteRecord *write_record = write_set->back();
        write_set->pop_back();

        auto &tab_name = write_record->GetTableName();
        auto file_handle = sm_manager_->fhs_.at(tab_name).get();
        auto &rid = write_record->GetRid();

        if (write_record->GetWriteType() == WType::INSERT_TUPLE) {
            // 部分 INSERT 失败时某些索引项可能尚未写入；缺失可接受，其它结构错误上抛。
            IndexHelper::DeleteAllIfPresent(sm_manager_, tab_name, write_record->GetNewRecord(), txn);
            file_handle->delete_record_quarantine(rid);
        } else if (write_record->GetWriteType() == WType::DELETE_TUPLE) {
            // Q9 DELETE 为逻辑删除，heap 正常仍在；若只差 heap 发布则按原 RID 幂等补回。
            try {
                file_handle->update_record(rid, const_cast<char *>(write_record->GetOldRecord().data), nullptr);
            } catch (const RecordNotFoundError &) {
                file_handle->insert_record(rid, const_cast<char *>(write_record->GetOldRecord().data));
            }
            IndexHelper::InsertAllIfMissingStrict(sm_manager_, tab_name, rid, write_record->GetOldRecord(), txn);
        } else {
            // 只撤销正向 UPDATE 真正改动的索引。未变化索引始终留在树中，避免回滚
            // 窗口内出现既不在 B+ 树、也未登记 stale 候选的并发漏读。
            IndexHelper::DeleteChangedIfPresent(sm_manager_, tab_name, write_record->GetOldRecord(),
                                                write_record->GetNewRecord(), txn);
            file_handle->update_record(rid, const_cast<char *>(write_record->GetOldRecord().data), nullptr);
            IndexHelper::InsertChangedIfMissingStrict(sm_manager_, tab_name, rid, write_record->GetOldRecord(),
                                                      write_record->GetNewRecord(), txn);
        }

        delete write_record;
    }

    // UPDATE 回滚期间保留旧键的 stale 候选，直到旧索引键重新插入完成；
    // 这样并发快照读始终能从 B+ 树或 stale registry 至少一处找到该 RID。
    CleanupStaleIndexAbort(txn);

    // 唯一键预留必须覆盖完整的 heap/index 回滚窗口。若先释放预留，另一事务可能
    // 复用同一键，而本事务随后按键删除索引项时会误删新事务刚写入的项。此处失败
    // 直接 fail-stop，保留预留比在回滚未完成时对外开放键空间更安全。
    CleanupUniqueKeyAbort(txn);

    std::array<std::vector<size_t>, kTupleStateShardCount> tuple_state_groups;
    for (size_t i = 0; i < tuple_states_snapshot.size(); ++i) {
        tuple_state_groups[TupleStateShardIndex(tuple_states_snapshot[i].first)].push_back(i);
    }
    std::vector<TupleKey> tuple_state_gc_keys;
    tuple_state_gc_keys.reserve(tuple_states_snapshot.size());
    for (const auto &group : tuple_state_groups) {
        for (size_t entry_idx : group) {
            tuple_state_gc_keys.push_back(tuple_states_snapshot[entry_idx].first);
        }
    }
    txn->SetTupleStateGcKeys(std::move(tuple_state_gc_keys));
    for (size_t shard_idx = 0; shard_idx < kTupleStateShardCount; ++shard_idx) {
        if (tuple_state_groups[shard_idx].empty()) {
            continue;
        }
        auto &shard = tuple_state_shards_[shard_idx];
        std::unique_lock<std::shared_mutex> lock(shard.mutex);
        for (size_t entry_idx : tuple_state_groups[shard_idx]) {
            auto &entry = tuple_states_snapshot[entry_idx];
            const TupleKey &key = entry.first;
            const TxnTupleWriteState &write_state = entry.second;
            if (write_state.inserted_by_txn && !write_state.has_before_state) {
                EraseTupleStateLocked(shard, key);
                continue;
            }
            if (write_state.has_before_state) {
                AssignTupleStateLocked(shard, key, write_state.before_first_write);
            } else {
                EraseTupleStateLocked(shard, key);
            }
        }
    }
    txn->ClearTupleWriteStates();
    GlobalConflictWaiter().NotifyRelease();
    // 回滚完成后释放全部锁，其他事务才能继续访问相关数据
    auto lock_set = txn->get_lock_set();
    for (const auto &lock_data_id : *lock_set) {
        lock_manager_->unlock(txn, lock_data_id);
    }
    lock_set->clear();

    // 回滚结局同样需要刷出日志，保证崩溃恢复能看到事务已终止
    if (log_manager != nullptr) {
        log_manager->flush_log_to_disk();
    }
    running_txns_.RemoveTxn(txn->get_start_ts());
    FinishActiveTransactionForCheckpoint();
    GarbageCollectStaleIndex();
    GarbageCollectSerializableMetadata();
    // 已中止事务的写入已回滚、其 undo 日志不再被任何版本链引用，用 start_ts 作为
    // reclaim_ts 即可（一旦它不再是最旧活跃事务即可安全回收）。
    RetireTransaction(txn, txn->get_start_ts());
    } catch (...) {
        // abort 尚未完成时绝不能提前减少 active_txn_count_ 让 checkpoint 误入静默态；
        // 失败即停机，恢复阶段会把没有 durable COMMIT 的事务按 loser 完整撤销。
        std::terminate();
    }
}

timestamp_t TransactionManager::ReserveCommitTimestamp() {
    std::lock_guard<std::mutex> lock(commit_publication_mutex_);
    // 旧版本 begin 也会消耗 timestamp，checkpoint 中 next_timestamp 可能显著大于
    // last_commit_ts。恢复后绝不能把逻辑时钟向下覆盖；数值空洞不代表未完成提交，
    // publication 只需按真实 reservation 的有序集合依次推进。
    timestamp_t commit_ts = commit_publications_.empty()
                                ? std::max(next_timestamp_.load(std::memory_order_relaxed),
                                           last_commit_ts_.load(std::memory_order_relaxed) + 1)
                                : next_timestamp_.load(std::memory_order_relaxed);
    auto [it, inserted] = commit_publications_.emplace(commit_ts, false);
    if (!inserted) {
        throw InternalError("Duplicate commit timestamp reservation");
    }
    next_timestamp_.store(commit_ts + 1, std::memory_order_relaxed);
    return commit_ts;
}

void TransactionManager::CancelCommitTimestamp(timestamp_t commit_ts) {
    {
        std::lock_guard<std::mutex> lock(commit_publication_mutex_);
        auto it = commit_publications_.find(commit_ts);
        if (it == commit_publications_.end() || it->second) {
            throw InternalError("Invalid commit timestamp cancellation");
        }
        commit_publications_.erase(it);
    }
    // 数值空洞合法；唤醒可能正等待被取消前缀的后续 SI 提交。
    commit_publication_cv_.notify_all();
}

std::unique_lock<std::mutex> TransactionManager::AwaitCommitPublicationTurn(timestamp_t commit_ts) {
    std::unique_lock<std::mutex> lock(commit_publication_mutex_);
    auto current = commit_publications_.find(commit_ts);
    if (current == commit_publications_.end()) {
        throw InternalError("Commit timestamp publication without reservation");
    }
    current->second = true;
    commit_publication_cv_.wait(lock, [&] {
        return !commit_publications_.empty() && commit_publications_.begin()->first == commit_ts;
    });
    return lock;
}

void TransactionManager::CompleteCommitPublication(
    timestamp_t commit_ts, std::unique_lock<std::mutex> &publication_lock) {
    if (!publication_lock.owns_lock() || commit_publications_.empty() ||
        commit_publications_.begin()->first != commit_ts) {
        throw InternalError("Commit publication completed out of order");
    }
    auto current = commit_publications_.find(commit_ts);
    if (current == commit_publications_.end() || !current->second) {
        throw InternalError("Commit publication completed without durable reservation");
    }
    commit_publications_.erase(current);
    last_commit_ts_.store(commit_ts, std::memory_order_release);
    publication_lock.unlock();
    commit_publication_cv_.notify_all();
}

void TransactionManager::FinishActiveTransactionForCheckpoint() {
    {
        std::lock_guard<std::mutex> checkpoint_lock(checkpoint_mutex_);
        if (active_txn_count_ > 0) {
            active_txn_count_--;
        }
    }
    checkpoint_cv_.notify_all();
}

bool TransactionManager::HasActiveTransaction(txn_id_t ignore_txn_id) {
    std::shared_lock<std::shared_mutex> lock(txn_map_mutex_);
    for (auto &entry : txn_map) {
        Transaction *txn = entry.second;
        if (txn == nullptr || txn->get_transaction_id() == ignore_txn_id) {
            continue;
        }
        if (txn->get_state() != TransactionState::COMMITTED &&
            txn->get_state() != TransactionState::ABORTED) {
            return true;
        }
    }
    return false;
}

void TransactionManager::RetireTransaction(Transaction *txn, timestamp_t reclaim_ts) {
    if (txn == nullptr) {
        return;
    }
    // watermark 为当前系统最低活跃读时间戳（无活跃事务时为最新 commit_ts）。
    // 只有 reclaim_ts <= watermark 的已结束事务才能安全释放：此时不存在（也不会
    // 再产生）start_ts < reclaim_ts 的活跃事务，因而没有任何读者会经由 undo 链
    // GetUndoLogOptional(link) -> FindTransaction(该事务) 访问它的 undo 日志。
    timestamp_t watermark = running_txns_.GetWatermark();
    std::vector<RetiredTransaction> to_retire;
    {
        std::unique_lock<std::mutex> lock(latch_);
        std::shared_lock<std::shared_mutex> map_lock(txn_map_mutex_);
        reclaimable_txns_.emplace(reclaim_ts, txn->get_transaction_id());
        auto reclaim_end = reclaimable_txns_.upper_bound(watermark);
        for (auto it = reclaimable_txns_.begin(); it != reclaim_end; ++it) {
            auto txn_it = txn_map.find(it->second);
            if (txn_it != txn_map.end()) {
                std::uint64_t retire_epoch =
                    version_gc_epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
                // txn_map 必须与对象一起等到 reader epoch 安全后再摘除；否则 guard
                // 只能防 UAF，却防不住 FindTransaction 在临界窗口返回空。
                to_retire.push_back(RetiredTransaction{retire_epoch, it->second, txn_it->second});
            }
        }
        reclaimable_txns_.erase(reclaimable_txns_.begin(), reclaim_end);
    }
    if (!to_retire.empty()) {
        // reclaim_ts 越过 watermark 后，任何现存或未来快照都不再需要这些事务产生的
        // 稳定 tuple-state/undo。先按写集增量压平非删除状态，再进入对象 epoch 回收。
        GarbageCollectTupleStatesForRetired(to_retire, watermark);
        {
            std::lock_guard<std::mutex> retired_lock(retired_txns_mutex_);
            pending_delete_txns_.insert(pending_delete_txns_.end(), to_retire.begin(), to_retire.end());
            pending_delete_txn_count_.store(pending_delete_txns_.size(), std::memory_order_seq_cst);
        }
        TryDrainRetiredTransactions();
    }
}

void TransactionManager::GarbageCollectTupleStatesForRetired(
    const std::vector<RetiredTransaction> &retired, timestamp_t watermark) {
    for (const auto &entry : retired) {
        if (entry.txn == nullptr) {
            continue;
        }
        // key 在 commit/abort 的准备阶段已按 shard 排序，退休热路径无需再复制
        // TxnTupleWriteState、构造 64 个临时向量或排序。
        auto keys = entry.txn->TakeTupleStateGcKeys();
        size_t begin = 0;
        while (begin < keys.size()) {
            const size_t shard_idx = TupleStateShardIndex(keys[begin]);
            size_t end = begin + 1;
            while (end < keys.size() && TupleStateShardIndex(keys[end]) == shard_idx) {
                ++end;
            }
            auto &shard = tuple_state_shards_[shard_idx];
            std::unique_lock<std::shared_mutex> lock(shard.mutex);
            for (size_t i = begin; i < end; ++i) {
                const auto &key = keys[i];
                auto state_it = shard.states.find(key);
                if (state_it == shard.states.end()) {
                    continue;
                }
                const TupleState &state = state_it->second;
                if (state.owner == INVALID_TXN_ID && !state.is_deleted && state.commit_ts <= watermark) {
                    // 之后的写者可把当前 heap 作为新的稳定基线；时间戳压平为 0 不会
                    // 改变任何可能存在的快照可见性或 FCW 判定。
                    EraseTupleStateLocked(shard, key);
                }
            }
            begin = end;
        }
    }
}

bool TransactionManager::BeginStaticCheckpoint(size_t ignored_active_txn_count) {
    std::unique_lock<std::mutex> checkpoint_lock(checkpoint_mutex_);
    checkpoint_in_progress_ = true;
    bool drained = checkpoint_cv_.wait_for(checkpoint_lock, std::chrono::milliseconds(50),
                                           [&] { return active_txn_count_ <= ignored_active_txn_count; });
    if (!drained) {
        checkpoint_in_progress_ = false;
        checkpoint_lock.unlock();
        checkpoint_cv_.notify_all();
        return false;
    }
    return true;
}

void TransactionManager::EndStaticCheckpoint() {
    {
        std::lock_guard<std::mutex> checkpoint_lock(checkpoint_mutex_);
        checkpoint_in_progress_ = false;
    }
    checkpoint_cv_.notify_all();
}

std::optional<TupleState> TransactionManager::GetTupleState(const TupleKey &key) {
    auto &shard = ShardFor(key);
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    auto it = shard.states.find(key);
    if (it == shard.states.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<RmRecord> TransactionManager::GetVisibleTuple(int fd, const Rid &rid,
                                                            const RmRecord &latest_physical,
                                                            Transaction *reader) {
    VersionReadGuard version_guard(this);
    auto visible = ResolveVisibleTupleImpl(fd, nullptr, rid, std::make_unique<RmRecord>(latest_physical), reader);
    if (visible == nullptr) {
        return std::nullopt;
    }
    return std::move(*visible);
}

std::unique_ptr<RmRecord> TransactionManager::ResolveVisibleTupleInReadGuard(
    RmFileHandle *fh, const Rid &rid, std::unique_ptr<RmRecord> latest_physical, Transaction *reader) {
    if (fh == nullptr) {
        throw InternalError("Visible tuple resolution requires a file handle");
    }
    return ResolveVisibleTupleImpl(fh->GetFd(), fh, rid, std::move(latest_physical), reader);
}

std::unique_ptr<RmRecord> TransactionManager::ReadVisibleTuple(
    RmFileHandle *fh, const Rid &rid, Context *context, Transaction *reader) {
    if (fh == nullptr) {
        throw InternalError("Visible tuple read requires a file handle");
    }
    TupleKey key{fh->GetFd(), rid};
    while (true) {
        VersionReadGuard version_guard(this);
        bool had_mvcc_state = fh->HasMvccState();
        std::uint64_t before = had_mvcc_state ? GetTuplePublicationEpoch(key) : 0;
        std::unique_ptr<RmRecord> latest;
        try {
            latest = fh->get_record(rid, context);
        } catch (const RecordNotFoundError &) {
            // 物理 slot 不存在也必须参与同一代次校验：abort insert / checkpoint
            // vacuum 可能恰在读取期间切换 state 与 heap，跨代时重试后再判空。
            if ((!had_mvcc_state && fh->HasMvccState()) ||
                (had_mvcc_state && before != GetTuplePublicationEpoch(key))) {
                continue;
            }
            return nullptr;
        }
        if (!had_mvcc_state) {
            // 写者先置 file flag、再发布 state、最后修改 heap。若 flag 在 heap 读取
            // 期间从 false 变 true，必须重读，不能把新 heap 当成无 MVCC 的基线。
            if (!fh->HasMvccState()) {
                return latest;
            }
            continue;
        }
        auto visible = ResolveVisibleTupleInReadGuard(fh, rid, std::move(latest), reader);
        std::uint64_t after = GetTuplePublicationEpoch(key);
        if (before == after) {
            return visible;
        }
        // state publication 跨过 heap copy：重新获取二者，避免 abort/commit 的混代快照。
    }
}

std::unique_ptr<RmRecord> TransactionManager::ResolveVisibleTupleImpl(
    int fd, RmFileHandle *fh, const Rid &rid, std::unique_ptr<RmRecord> latest_physical, Transaction *reader) {
    // 调用方已经在 PageReadGuard 下取得 latest；写者发布 tuple state 后才允许修改 heap。
    if (fh != nullptr && !fh->HasMvccState()) {
        return latest_physical;
    }
    TupleKey key{fd, rid};
    auto state_opt = GetTupleState(key);
    if (!state_opt.has_value()) {
        return latest_physical;
    }

    TupleState state = state_opt.value();
    txn_id_t reader_id = reader == nullptr ? INVALID_TXN_ID : reader->get_transaction_id();
    timestamp_t read_ts = reader == nullptr ? last_commit_ts_.load() : reader->get_start_ts();

    if (state.owner == reader_id) {
        if (state.is_deleted) {
            return nullptr;
        }
        return latest_physical;
    }

    if (state.owner != INVALID_TXN_ID) {
        if (!state.undo_head.has_value()) {
            return nullptr;
        }
        UndoLink link = state.undo_head.value();
        while (link.IsValid()) {
            auto undo_opt = GetUndoLogOptional(link);
            if (!undo_opt.has_value()) {
                return nullptr;
            }
            UndoLog undo = std::move(undo_opt.value());
            if (undo.ts_ <= read_ts) {
                if (undo.is_deleted_) {
                    return nullptr;
                }
                return std::make_unique<RmRecord>(std::move(undo.record_));
            }
            link = undo.prev_version_;
        }
        return nullptr;
    }

    if (state.commit_ts <= read_ts) {
        if (state.is_deleted) {
            return nullptr;
        }
        return latest_physical;
    }

    if (!state.undo_head.has_value()) {
        return nullptr;
    }
    UndoLink link = state.undo_head.value();
    while (link.IsValid()) {
        auto undo_opt = GetUndoLogOptional(link);
        if (!undo_opt.has_value()) {
            return nullptr;
        }
        UndoLog undo = std::move(undo_opt.value());
        if (undo.ts_ <= read_ts) {
            if (undo.is_deleted_) {
                return nullptr;
            }
            return std::make_unique<RmRecord>(std::move(undo.record_));
        }
        link = undo.prev_version_;
    }
    return nullptr;
}

WriteCheckOutcome TransactionManager::CheckAndAcquireWrite(RmFileHandle *fh, const Rid &rid, Transaction *txn) {
    if (txn == nullptr || fh == nullptr) {
        throw InternalError("Write conflict check requires a transaction");
    }
    fh->MarkHasMvccState();
    int fd = fh->GetFd();
    TupleKey key{fd, rid};
    auto &shard = ShardFor(key);
    std::unique_lock<std::shared_mutex> lock(shard.mutex);
    auto [it, inserted] = shard.states.try_emplace(key, TupleState{});
    if (inserted) {
        size_t size = shard.approx_size.fetch_add(1, std::memory_order_release) + 1;
        ObserveTupleStateShardSize(size);
    }
    TupleState &state = it->second;
    txn_id_t txn_id = txn->get_transaction_id();

    if (state.owner == txn_id) {
        return {WriteCheckResult::OK, false, INVALID_TXN_ID};
    }
    if (state.owner != INVALID_TXN_ID) {
        return {WriteCheckResult::CONFLICT_WITH_ACTIVE_WRITER, false, state.owner};
    }
    if (state.commit_ts > txn->get_start_ts()) {
        return {WriteCheckResult::CONFLICT_WITH_COMMITTED_VERSION, false, INVALID_TXN_ID};
    }
    if (state.is_deleted && state.commit_ts <= txn->get_start_ts()) {
        return {WriteCheckResult::NOT_VISIBLE_OR_DELETED, false, INVALID_TXN_ID};
    }

    state.owner = txn_id;
    shard.publication_epoch.fetch_add(1, std::memory_order_release);
    return {WriteCheckResult::OK, true, INVALID_TXN_ID};
}

WriteCheckOutcome TransactionManager::AcquireWriteAndRecord(RmFileHandle *fh, const Rid &rid, const RmRecord &old_record,
                                                            Transaction *txn, TupleWriteKind kind) {
    if (txn == nullptr || fh == nullptr) {
        throw InternalError("Write conflict check requires a transaction");
    }
    fh->MarkHasMvccState();
    int fd = fh->GetFd();
    TupleKey key{fd, rid};
    auto &shard = ShardFor(key);
    std::unique_lock<std::shared_mutex> lock(shard.mutex);
    auto [it, inserted] = shard.states.try_emplace(key, TupleState{});
    if (inserted) {
        size_t size = shard.approx_size.fetch_add(1, std::memory_order_release) + 1;
        ObserveTupleStateShardSize(size);
    }
    TupleState &state = it->second;
    txn_id_t txn_id = txn->get_transaction_id();

    if (state.owner == txn_id) {
        const bool previous_is_deleted = state.is_deleted;
        state.is_deleted = kind == TupleWriteKind::DELETE;
        if (previous_is_deleted != state.is_deleted) {
            shard.publication_epoch.fetch_add(1, std::memory_order_release);
        }
        return {WriteCheckResult::OK, false, INVALID_TXN_ID, false,
                previous_is_deleted != state.is_deleted, previous_is_deleted};
    }
    if (state.owner != INVALID_TXN_ID) {
        return {WriteCheckResult::CONFLICT_WITH_ACTIVE_WRITER, false, state.owner, false};
    }
    if (state.commit_ts > txn->get_start_ts()) {
        return {WriteCheckResult::CONFLICT_WITH_COMMITTED_VERSION, false, INVALID_TXN_ID, false};
    }
    if (state.is_deleted && state.commit_ts <= txn->get_start_ts()) {
        return {WriteCheckResult::NOT_VISIBLE_OR_DELETED, false, INVALID_TXN_ID, false};
    }

    bool first_write = !txn->HasTupleWriteState(key);
    if (first_write) {
        TxnTupleWriteState write_state;
        write_state.inserted_by_txn = false;
        write_state.has_before_state = true;
        write_state.before_first_write = state;

        UndoLog undo;
        undo.is_deleted_ = state.is_deleted;
        undo.record_ = old_record;
        undo.ts_ = state.commit_ts;
        if (state.undo_head.has_value()) {
            undo.prev_version_ = state.undo_head.value();
        }
        UndoLink undo_link = txn->AppendUndoLog(std::move(undo));
        state.undo_head = undo_link;
        txn->SetTupleWriteState(key, write_state);
    }

    state.owner = txn_id;
    state.is_deleted = kind == TupleWriteKind::DELETE;
    shard.publication_epoch.fetch_add(1, std::memory_order_release);
    return {WriteCheckResult::OK, true, INVALID_TXN_ID, first_write};
}

void TransactionManager::ReleaseWriteOwners(const std::vector<TupleKey> &keys, Transaction *txn) {
    if (txn == nullptr) {
        return;
    }
    std::array<std::vector<const TupleKey *>, kTupleStateShardCount> tuple_state_groups;
    for (const auto &key : keys) {
        tuple_state_groups[TupleStateShardIndex(key)].push_back(&key);
    }
    for (size_t shard_idx = 0; shard_idx < kTupleStateShardCount; ++shard_idx) {
        if (tuple_state_groups[shard_idx].empty()) {
            continue;
        }
        auto &shard = tuple_state_shards_[shard_idx];
        std::unique_lock<std::shared_mutex> lock(shard.mutex);
        for (const TupleKey *key : tuple_state_groups[shard_idx]) {
            auto it = shard.states.find(*key);
            if (it != shard.states.end() && it->second.owner == txn->get_transaction_id()) {
                it->second.owner = INVALID_TXN_ID;
                shard.publication_epoch.fetch_add(1, std::memory_order_release);
            }
        }
    }
    GlobalConflictWaiter().NotifyRelease();
}

std::uint64_t TransactionManager::TupleStateShardHash(const TupleKey &key) {
    std::uint64_t value = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.fd)) << 32) ^
                          (static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.rid.page_no)) << 16) ^
                          static_cast<std::uint16_t>(key.rid.slot_no);
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33;
    return value;
}

size_t TransactionManager::TupleStateShardIndex(const TupleKey &key) {
    return TupleStateShardHash(key) & (kTupleStateShardCount - 1);
}

std::uint64_t TransactionManager::GetTuplePublicationEpoch(const TupleKey &key) const {
    return ShardFor(key).publication_epoch.load(std::memory_order_acquire);
}

TransactionManager::TupleStateShard &TransactionManager::ShardFor(const TupleKey &key) {
    return tuple_state_shards_[TupleStateShardIndex(key)];
}

const TransactionManager::TupleStateShard &TransactionManager::ShardFor(const TupleKey &key) const {
    return tuple_state_shards_[TupleStateShardIndex(key)];
}

void TransactionManager::AssignTupleStateLocked(TupleStateShard &shard, const TupleKey &key,
                                                const TupleState &state) {
    auto [it, inserted] = shard.states.try_emplace(key, state);
    if (inserted) {
        size_t size = shard.approx_size.fetch_add(1, std::memory_order_release) + 1;
        ObserveTupleStateShardSize(size);
        shard.publication_epoch.fetch_add(1, std::memory_order_release);
        return;
    }
    it->second = state;
    shard.publication_epoch.fetch_add(1, std::memory_order_release);
}

bool TransactionManager::EraseTupleStateLocked(TupleStateShard &shard, const TupleKey &key) {
    auto erased = shard.states.erase(key);
    if (erased > 0) {
        shard.approx_size.fetch_sub(1, std::memory_order_release);
        shard.publication_epoch.fetch_add(1, std::memory_order_release);
        return true;
    }
    return false;
}

void TransactionManager::ObserveTupleStateShardSize(size_t size) {
    (void)size;
}

void TransactionManager::PrintTupleStateStats() const {
}

std::uint64_t TransactionManager::StaleIndexRegistryId(int fd, int index_no) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(fd)) << 32) ^
           static_cast<std::uint32_t>(index_no);
}

size_t TransactionManager::StaleIndexBucketIndex(int fd, int index_no) {
    // 完整混合 fd/index_no；旧实现直接取低位会让不同表的第 0 个索引全部落入同一桶。
    std::uint64_t mixed = StaleIndexRegistryId(fd, index_no);
    mixed ^= mixed >> 33U;
    mixed *= 0xff51afd7ed558ccdULL;
    mixed ^= mixed >> 33U;
    mixed *= 0xc4ceb9fe1a85ec53ULL;
    mixed ^= mixed >> 33U;
    return static_cast<size_t>(mixed) & (kStaleIndexBucketCount - 1);
}

TransactionManager::StaleIndexBucket &TransactionManager::StaleIndexBucketFor(int fd, int index_no) {
    return stale_index_buckets_[StaleIndexBucketIndex(fd, index_no)];
}

const TransactionManager::StaleIndexBucket &TransactionManager::StaleIndexBucketFor(int fd, int index_no) const {
    return stale_index_buckets_[StaleIndexBucketIndex(fd, index_no)];
}

StaleIndexEntry TransactionManager::AddStaleIndexEntry(int fd, int index_no,
                                                       const std::vector<char> &encoded_key,
                                                       const Rid &rid, Transaction *txn) {
    if (txn == nullptr) {
        throw InternalError("Stale index entry requires a transaction");
    }
    StaleIndexEntry entry;
    entry.key = StaleIndexKey{fd, index_no, std::string(encoded_key.data(), encoded_key.size())};
    entry.tuple = TupleKey{fd, rid};
    entry.creating_txn = txn->get_transaction_id();
    entry.retire_ts = INVALID_TS;
    auto &bucket = StaleIndexBucketFor(fd, index_no);
    const size_t bucket_index = StaleIndexBucketIndex(fd, index_no);
    const std::uint64_t bucket_bit = std::uint64_t{1} << bucket_index;

    // 先预占计数：读线程观察到 0 时，可以确定既没有已发布条目，也没有正在发布的条目。
    bucket.entry_count.fetch_add(1, std::memory_order_acq_rel);
    try {
        std::unique_lock<std::shared_mutex> lock(bucket.mutex);
        stale_index_active_mask_.fetch_or(bucket_bit, std::memory_order_release);

        auto equal_it = bucket.equal_entries.try_emplace(entry.key).first;
        bool equal_pushed = false;
        try {
            equal_it->second.push_back(entry);
            equal_pushed = true;

            const auto registry_id = StaleIndexRegistryId(fd, index_no);
            auto index_it = bucket.entries_by_index.try_emplace(registry_id).first;
            try {
                auto key_it = index_it->second.try_emplace(entry.key.encoded_key).first;
                bool range_pushed = false;
                try {
                    key_it->second.push_back(entry);
                    range_pushed = true;
                    // txn list 与两份 registry 一起发布；后续 commit/abort 以此列表定位条目。
                    txn->AddStaleIndexEntry(entry);
                } catch (...) {
                    if (range_pushed) {
                        key_it->second.pop_back();
                    }
                    if (key_it->second.empty()) {
                        index_it->second.erase(key_it);
                    }
                    throw;
                }
            } catch (...) {
                if (index_it->second.empty()) {
                    bucket.entries_by_index.erase(index_it);
                }
                throw;
            }
        } catch (...) {
            if (equal_pushed) {
                equal_it->second.pop_back();
            }
            if (equal_it->second.empty()) {
                bucket.equal_entries.erase(equal_it);
            }
            throw;
        }
    } catch (...) {
        std::unique_lock<std::shared_mutex> lock(bucket.mutex);
        size_t previous = bucket.entry_count.fetch_sub(1, std::memory_order_acq_rel);
        if (previous == 0) {
            bucket.entry_count.fetch_add(1, std::memory_order_relaxed);
            throw InternalError("Stale index entry count underflow");
        }
        if (previous == 1 && bucket.equal_entries.empty() && bucket.entries_by_index.empty()) {
            stale_index_active_mask_.fetch_and(~bucket_bit, std::memory_order_release);
        }
        throw;
    }
    return entry;
}

void TransactionManager::RemoveStaleIndexEntries(const std::vector<StaleIndexEntry> &entries, Transaction *txn) {
    if (entries.empty()) {
        return;
    }
    for (const auto &entry : entries) {
        auto &bucket = StaleIndexBucketFor(entry.key.fd, entry.key.index_no);
        const size_t bucket_index = StaleIndexBucketIndex(entry.key.fd, entry.key.index_no);
        const std::uint64_t bucket_bit = std::uint64_t{1} << bucket_index;
        std::unique_lock<std::shared_mutex> lock(bucket.mutex);
        // 调用方已先把 abort 前的旧键补回 B+ 树。删除 stale 候选前发布 handoff
        // epoch，使“先扫树、后查 stale”的并发读者能检测到来源切换并重扫树。
        bucket.handoff_epoch.fetch_add(1, std::memory_order_release);
        auto remove_entry = [&](std::vector<StaleIndexEntry> &candidates) -> size_t {
            size_t old_size = candidates.size();
            candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                [&](const StaleIndexEntry &candidate) {
                    return candidate.key == entry.key && candidate.tuple == entry.tuple &&
                           candidate.creating_txn == entry.creating_txn;
                }), candidates.end());
            return old_size - candidates.size();
        };

        size_t removed_count = 0;
        auto equal_it = bucket.equal_entries.find(entry.key);
        if (equal_it != bucket.equal_entries.end()) {
            removed_count = remove_entry(equal_it->second);
            if (equal_it->second.empty()) {
                bucket.equal_entries.erase(equal_it);
            }
        }

        auto index_it = bucket.entries_by_index.find(StaleIndexRegistryId(entry.key.fd, entry.key.index_no));
        if (index_it != bucket.entries_by_index.end()) {
            auto key_it = index_it->second.find(entry.key.encoded_key);
            if (key_it != index_it->second.end()) {
                remove_entry(key_it->second);
                if (key_it->second.empty()) {
                    index_it->second.erase(key_it);
                }
            }
            if (index_it->second.empty()) {
                bucket.entries_by_index.erase(index_it);
            }
        }
        if (removed_count > 0) {
            size_t previous = bucket.entry_count.fetch_sub(removed_count, std::memory_order_acq_rel);
            if (previous < removed_count) {
                bucket.entry_count.fetch_add(removed_count, std::memory_order_relaxed);
                throw InternalError("Stale index entry count underflow");
            }
            const size_t new_count = previous - removed_count;
            if (new_count == 0 && bucket.equal_entries.empty() && bucket.entries_by_index.empty()) {
                stale_index_active_mask_.fetch_and(~bucket_bit, std::memory_order_release);
            }
        }
    }
    if (txn != nullptr) {
        txn->RemoveStaleIndexEntries(entries);
    }
}

std::uint64_t TransactionManager::GetStaleIndexHandoffEpoch(int fd, int index_no) const {
    return StaleIndexBucketFor(fd, index_no).handoff_epoch.load(std::memory_order_acquire);
}

bool TransactionManager::StaleIndexEntryVisibleToReader(const StaleIndexEntry &entry, Transaction *reader) const {
    if (entry.retire_ts == INVALID_TS) {
        return true;
    }
    timestamp_t read_ts = reader == nullptr ? last_commit_ts_.load() : reader->get_start_ts();
    return entry.retire_ts > read_ts;
}

std::vector<Rid> TransactionManager::LookupStaleIndexEqual(int fd, int index_no,
                                                           const std::vector<char> &encoded_key,
                                                           Transaction *reader) {
    std::vector<Rid> result;
    const auto &bucket = StaleIndexBucketFor(fd, index_no);
    if (bucket.entry_count.load(std::memory_order_acquire) == 0) {
        return result;
    }
    std::string key(encoded_key.data(), encoded_key.size());
    std::shared_lock<std::shared_mutex> lock(bucket.mutex);
    auto it = bucket.equal_entries.find(StaleIndexKey{fd, index_no, key});
    if (it == bucket.equal_entries.end()) {
        return result;
    }
    for (const auto &entry : it->second) {
        if (StaleIndexEntryVisibleToReader(entry, reader)) {
            result.push_back(entry.tuple.rid);
        }
    }
    return result;
}

std::vector<Rid> TransactionManager::LookupStaleIndexRange(int fd, int index_no,
                                                           const std::vector<char> &lower_key,
                                                           const std::vector<char> &upper_key,
                                                           const std::vector<ColType> &col_types,
                                                           const std::vector<int> &col_lens,
                                                           Transaction *reader) {
    auto bounded = LookupStaleIndexRangeBounded(fd, index_no, lower_key, upper_key, col_types, col_lens,
                                                std::numeric_limits<size_t>::max(), reader);
    return std::move(bounded.first);
}

std::pair<std::vector<Rid>, bool> TransactionManager::LookupStaleIndexRangeBounded(
    int fd, int index_no, const std::vector<char> &lower_key, const std::vector<char> &upper_key,
    const std::vector<ColType> &col_types, const std::vector<int> &col_lens,
    size_t max_results, Transaction *reader) {
    std::vector<Rid> result;
    const auto &bucket = StaleIndexBucketFor(fd, index_no);
    if (bucket.entry_count.load(std::memory_order_acquire) == 0) {
        return {std::move(result), false};
    }
    std::shared_lock<std::shared_mutex> lock(bucket.mutex);
    auto index_it = bucket.entries_by_index.find(StaleIndexRegistryId(fd, index_no));
    if (index_it == bucket.entries_by_index.end()) {
        return {std::move(result), false};
    }
    for (const auto &key_entry : index_it->second) {
        const char *key = key_entry.first.data();
        if (ix_compare(key, lower_key.data(), col_types, col_lens) < 0 ||
            ix_compare(key, upper_key.data(), col_types, col_lens) > 0) {
            continue;
        }
        for (const auto &entry : key_entry.second) {
            if (StaleIndexEntryVisibleToReader(entry, reader)) {
                if (result.size() >= max_results) {
                    return {std::move(result), true};
                }
                result.push_back(entry.tuple.rid);
            }
        }
    }
    return {std::move(result), false};
}

void TransactionManager::RemoveSerializableEdgesForTxnLocked(txn_id_t txn_id) {
    for (auto it = serializable_rw_edges_.begin(); it != serializable_rw_edges_.end();) {
        if (it->first == txn_id || it->second == txn_id) {
            auto out_it = serializable_rw_out_edges_.find(it->first);
            if (out_it != serializable_rw_out_edges_.end()) {
                out_it->second.erase(it->second);
                if (out_it->second.empty()) {
                    serializable_rw_out_edges_.erase(out_it);
                }
            }
            auto in_it = serializable_rw_in_edges_.find(it->second);
            if (in_it != serializable_rw_in_edges_.end()) {
                in_it->second.erase(it->first);
                if (in_it->second.empty()) {
                    serializable_rw_in_edges_.erase(in_it);
                }
            }
            it = serializable_rw_edges_.erase(it);
        } else {
            ++it;
        }
    }
}

void TransactionManager::RemoveSerializableTxnIndexesLocked(txn_id_t txn_id) {
    auto rid_it = serializable_rid_reads_.find(txn_id);
    if (rid_it != serializable_rid_reads_.end()) {
        for (const auto &key : rid_it->second) {
            auto index_it = serializable_rid_read_index_.find(key);
            if (index_it != serializable_rid_read_index_.end()) {
                index_it->second.erase(txn_id);
                if (index_it->second.empty()) {
                    serializable_rid_read_index_.erase(index_it);
                }
            }
        }
    }

    auto predicate_it = serializable_predicate_reads_.find(txn_id);
    if (predicate_it != serializable_predicate_reads_.end()) {
        for (const auto &predicate : predicate_it->second) {
            auto index_it = serializable_predicate_read_fd_index_.find(predicate.fd);
            if (index_it != serializable_predicate_read_fd_index_.end()) {
                index_it->second.erase(txn_id);
                if (index_it->second.empty()) {
                    serializable_predicate_read_fd_index_.erase(index_it);
                }
            }
        }
    }

    auto intent_it = serializable_write_intents_.find(txn_id);
    if (intent_it != serializable_write_intents_.end()) {
        for (const auto &intent : intent_it->second) {
            auto index_it = serializable_write_intent_fd_index_.find(intent.fd);
            if (index_it != serializable_write_intent_fd_index_.end()) {
                index_it->second.erase(txn_id);
                if (index_it->second.empty()) {
                    serializable_write_intent_fd_index_.erase(index_it);
                }
            }
        }
    }
}

void TransactionManager::RegisterSerializableBegin(Transaction *txn) {
    if (txn == nullptr || txn->get_isolation_level() != IsolationLevel::SERIALIZABLE) {
        return;
    }
    std::lock_guard<std::mutex> lock(ssi_mutex_);
    serializable_intervals_[txn->get_transaction_id()] =
        SerializableTxnInterval{txn->get_start_ts(), INVALID_TS, TransactionState::GROWING};
    // interval 与 present 必须在同一临界区发布，避免 GC 清标志后 begin 才插入 interval。
    ssi_metadata_present_.store(true, std::memory_order_release);
}

std::unique_lock<std::mutex> TransactionManager::PrepareSerializableCommit(Transaction *txn,
                                                                           timestamp_t commit_ts) {
    if (txn == nullptr || txn->get_isolation_level() != IsolationLevel::SERIALIZABLE) {
        return {};
    }
    std::unique_lock<std::mutex> lock(ssi_mutex_);
    if (serializable_intervals_.find(txn->get_transaction_id()) == serializable_intervals_.end()) {
        throw InternalError("Serializable transaction interval missing before commit");
    }

    // 只有 Tout 从 GROWING 变为 COMMITTED 会让既有 Tin->rw Pivot->rw Tout
    // 首次满足“Tout 先于 Tin 提交”。必须在 COMMIT WAL 前检查；durable 后已不能 abort。
    const txn_id_t tout = txn->get_transaction_id();
    auto pivots_it = serializable_rw_in_edges_.find(tout);
    if (pivots_it != serializable_rw_in_edges_.end()) {
        for (txn_id_t pivot : pivots_it->second) {
            auto tins_it = serializable_rw_in_edges_.find(pivot);
            if (tins_it == serializable_rw_in_edges_.end()) {
                continue;
            }
            for (txn_id_t tin : tins_it->second) {
                if (SerializableDangerousStructureLocked(tin, pivot, tout, commit_ts)) {
                    throw TransactionAbortException(tout, AbortReason::SSI_DANGEROUS_STRUCTURE);
                }
            }
        }
    }
    return lock;
}

void TransactionManager::MarkSerializableCommitLocked(Transaction *txn) {
    if (txn == nullptr || txn->get_isolation_level() != IsolationLevel::SERIALIZABLE) {
        return;
    }
    txn_id_t txn_id = txn->get_transaction_id();
    auto interval_it = serializable_intervals_.find(txn_id);
    if (interval_it == serializable_intervals_.end()) {
        throw InternalError("Serializable transaction interval missing at commit publication");
    }
    auto &interval = interval_it->second;
    interval.start_ts = txn->get_start_ts();
    interval.finish_ts = txn->get_commit_ts();
    interval.state = TransactionState::COMMITTED;
    auto intents_it = serializable_write_intents_.find(txn_id);
    if (intents_it != serializable_write_intents_.end()) {
        for (auto &intent : intents_it->second) {
            intent.committed = true;
            intent.writer_commit_ts = txn->get_commit_ts();
        }
    }
}

void TransactionManager::CleanupSerializableAbort(txn_id_t txn_id) {
    if (txn_id == INVALID_TXN_ID) {
        return;
    }
    if (!ssi_metadata_present_.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(ssi_mutex_);
    serializable_intervals_.erase(txn_id);
    RemoveSerializableTxnIndexesLocked(txn_id);
    serializable_rid_reads_.erase(txn_id);
    serializable_predicate_reads_.erase(txn_id);
    serializable_write_intents_.erase(txn_id);
    RemoveSerializableEdgesForTxnLocked(txn_id);
    if (serializable_intervals_.empty()) {
        ssi_metadata_present_.store(false, std::memory_order_release);
    }
}

void TransactionManager::GarbageCollectSerializableMetadata() {
    if (!ssi_metadata_present_.load(std::memory_order_acquire)) {
        return;
    }

    // 与 begin 固定采用 publication -> SSI 的锁顺序。这样“选择事务快照 + 登记
    // SERIALIZABLE 区间”相对 SSI GC 是一个原子动作，GC 不会在新事务取到旧快照
    // 后、区间尚未登记前，删掉该快照不可见的 writer intent。
    std::lock_guard<std::mutex> publication_lock(commit_publication_mutex_);
    std::lock_guard<std::mutex> lock(ssi_mutex_);
    const timestamp_t published_commit_ts = last_commit_ts_.load(std::memory_order_acquire);
    timestamp_t min_active_start = std::numeric_limits<timestamp_t>::max();
    bool has_active = false;
    for (const auto &entry : serializable_intervals_) {
        if (entry.second.state == TransactionState::GROWING) {
            has_active = true;
            min_active_start = std::min(min_active_start, entry.second.start_ts);
        }
    }

    std::vector<txn_id_t> removable;
    for (const auto &entry : serializable_intervals_) {
        if (entry.second.state != TransactionState::COMMITTED) {
            continue;
        }
        if (entry.second.finish_ts <= published_commit_ts &&
            (!has_active || entry.second.finish_ts <= min_active_start)) {
            removable.push_back(entry.first);
        }
    }

    for (txn_id_t txn_id : removable) {
        serializable_intervals_.erase(txn_id);
        RemoveSerializableTxnIndexesLocked(txn_id);
        serializable_rid_reads_.erase(txn_id);
        serializable_predicate_reads_.erase(txn_id);
        serializable_write_intents_.erase(txn_id);
        RemoveSerializableEdgesForTxnLocked(txn_id);
    }
    if (serializable_intervals_.empty()) {
        ssi_metadata_present_.store(false, std::memory_order_release);
    }
}

bool TransactionManager::SerializableCondsMatch(const std::vector<ColMeta> &cols,
                                                const std::vector<Condition> &conds,
                                                const RmRecord &record) const {
    for (const auto &cond : conds) {
        auto lhs_it = std::find_if(cols.begin(), cols.end(), [&](const ColMeta &col) {
            return col.tab_name == cond.lhs_col.tab_name && col.name == cond.lhs_col.col_name;
        });
        if (lhs_it == cols.end()) {
            return true;
        }
        const char *lhs = record.data + lhs_it->offset;
        const char *rhs = nullptr;
        Value rhs_value;
        if (cond.is_rhs_val) {
            rhs_value = cond.rhs_val;
            if (rhs_value.raw == nullptr) {
                rhs_value.init_raw(lhs_it->len);
            }
            rhs = rhs_value.raw->data;
        } else {
            auto rhs_it = std::find_if(cols.begin(), cols.end(), [&](const ColMeta &col) {
                return col.tab_name == cond.rhs_col.tab_name && col.name == cond.rhs_col.col_name;
            });
            if (rhs_it == cols.end()) {
                return true;
            }
            rhs = record.data + rhs_it->offset;
        }
        int cmp = CompareSerializableValue(lhs, rhs, lhs_it->type, lhs_it->len);
        if (!CheckSerializableOp(cmp, cond.op)) {
            return false;
        }
    }
    return true;
}

bool TransactionManager::SerializablePredicateMatches(const PredicateRead &predicate,
                                                      const RmRecord &record) const {
    if (predicate.full_table_scan || predicate.lossy) {
        return true;
    }
    return SerializableCondsMatch(predicate.cols, predicate.conds, record);
}

bool TransactionManager::SerializableWriteChangesRecord(const WriteIntent &intent) const {
    if (!intent.old_record.has_value() || !intent.new_record.has_value()) {
        return true;
    }
    const auto &old_record = intent.old_record.value();
    const auto &new_record = intent.new_record.value();
    return old_record.size != new_record.size || memcmp(old_record.data, new_record.data, old_record.size) != 0;
}

bool TransactionManager::SerializableWriteAffectsPredicate(const PredicateRead &predicate,
                                                           const WriteIntent &intent) const {
    if (predicate.fd != intent.fd) {
        return false;
    }
    bool old_matches = intent.old_record.has_value() &&
                       SerializablePredicateMatches(predicate, intent.old_record.value());
    bool new_matches = intent.new_record.has_value() &&
                       SerializablePredicateMatches(predicate, intent.new_record.value());
    if (old_matches != new_matches) {
        return true;
    }
    return old_matches && new_matches && SerializableWriteChangesRecord(intent);
}

bool TransactionManager::SerializableWriteVisibleToReader(const WriteIntent &intent,
                                                          timestamp_t reader_start_ts) const {
    return intent.committed && intent.writer_commit_ts != INVALID_TS && intent.writer_commit_ts <= reader_start_ts;
}

bool TransactionManager::AddSerializableRwEdgeAndCheck(txn_id_t from_reader, txn_id_t to_writer,
                                                       txn_id_t current_txn) {
    if (from_reader == INVALID_TXN_ID || to_writer == INVALID_TXN_ID || from_reader == to_writer) {
        return false;
    }
    if (!serializable_rw_edges_.emplace(from_reader, to_writer).second) {
        return false;
    }
    serializable_rw_out_edges_[from_reader].insert(to_writer);
    serializable_rw_in_edges_[to_writer].insert(from_reader);

    // Only the two chains that include this newly inserted edge are eligible to abort current_txn.
    auto incoming_it = serializable_rw_in_edges_.find(from_reader);
    if (incoming_it != serializable_rw_in_edges_.end()) {
        for (txn_id_t tin : incoming_it->second) {
            if (SerializableDangerousStructureLocked(tin, from_reader, to_writer)) {
                return true;
            }
        }
    }
    auto outgoing_it = serializable_rw_out_edges_.find(to_writer);
    if (outgoing_it != serializable_rw_out_edges_.end()) {
        for (txn_id_t tout : outgoing_it->second) {
            if (SerializableDangerousStructureLocked(from_reader, to_writer, tout)) {
                return true;
            }
        }
    }
    return false;
}

bool TransactionManager::SerializableDangerousStructureLocked(
    txn_id_t tin, txn_id_t tpivot, txn_id_t tout, timestamp_t assumed_tout_finish) const {
    auto tin_it = serializable_intervals_.find(tin);
    auto tpivot_it = serializable_intervals_.find(tpivot);
    auto tout_it = serializable_intervals_.find(tout);
    if (tin_it == serializable_intervals_.end() ||
        tpivot_it == serializable_intervals_.end() ||
        tout_it == serializable_intervals_.end()) {
        return false;
    }
    const bool assume_tout_committed = assumed_tout_finish != INVALID_TS;
    const timestamp_t tout_finish = assume_tout_committed ? assumed_tout_finish : tout_it->second.finish_ts;
    if (!SerializableIntervalsOverlap(tin_it->second.start_ts, tin_it->second.finish_ts,
                                      tpivot_it->second.start_ts, tpivot_it->second.finish_ts) ||
        !SerializableIntervalsOverlap(tpivot_it->second.start_ts, tpivot_it->second.finish_ts,
                                      tout_it->second.start_ts, tout_finish)) {
        return false;
    }
    if (tin == tout) {
        return true;
    }
    const bool tout_committed = assume_tout_committed ||
                                (tout_it->second.state == TransactionState::COMMITTED &&
                                 tout_it->second.finish_ts != INVALID_TS);
    const bool tin_committed = tin_it->second.state == TransactionState::COMMITTED &&
                               tin_it->second.finish_ts != INVALID_TS;
    return tout_committed && (!tin_committed || tout_finish < tin_it->second.finish_ts);
}

void TransactionManager::RecordSerializablePredicateRead(int fd, const std::string &table_name,
                                                         const std::vector<Condition> &conds,
                                                         const std::vector<ColMeta> &cols,
                                                         bool full_table_scan, bool lossy,
                                                         Transaction *txn) {
    if (txn == nullptr || txn->get_isolation_level() != IsolationLevel::SERIALIZABLE) {
        return;
    }
    std::lock_guard<std::mutex> lock(ssi_mutex_);
    PredicateRead predicate{txn->get_transaction_id(), fd, table_name, conds, cols, full_table_scan,
                            lossy, txn->get_start_ts()};
    txn_id_t reader_txn = txn->get_transaction_id();
    serializable_predicate_reads_[reader_txn].push_back(predicate);
    serializable_predicate_read_fd_index_[fd].insert(reader_txn);

    auto writer_index_it = serializable_write_intent_fd_index_.find(fd);
    if (writer_index_it == serializable_write_intent_fd_index_.end()) {
        return;
    }
    auto candidate_writers = writer_index_it->second;
    for (txn_id_t writer : candidate_writers) {
        if (writer == reader_txn) {
            continue;
        }
        auto writer_entry = serializable_write_intents_.find(writer);
        if (writer_entry == serializable_write_intents_.end()) {
            continue;
        }
        for (const auto &intent : writer_entry->second) {
            if (intent.fd != fd || SerializableWriteVisibleToReader(intent, txn->get_start_ts())) {
                continue;
            }
            if (SerializableWriteAffectsPredicate(predicate, intent) &&
                AddSerializableRwEdgeAndCheck(reader_txn, writer, reader_txn)) {
                throw TransactionAbortException(reader_txn,
                                                AbortReason::SSI_DANGEROUS_STRUCTURE);
            }
        }
    }
}

void TransactionManager::RecordSerializableTupleRead(int fd, const Rid &rid, Transaction *txn) {
    if (txn == nullptr || txn->get_isolation_level() != IsolationLevel::SERIALIZABLE) {
        return;
    }
    std::lock_guard<std::mutex> lock(ssi_mutex_);
    TupleKey key{fd, rid};
    if (serializable_rid_reads_[txn->get_transaction_id()].insert(key).second) {
        serializable_rid_read_index_[key].insert(txn->get_transaction_id());
    }
}

void TransactionManager::RecordSerializableWrite(int fd, const std::string &table_name, const Rid &rid,
                                                 const std::optional<RmRecord> &old_record,
                                                 const std::optional<RmRecord> &new_record,
                                                 const std::vector<ColMeta> &cols,
                                                 Transaction *txn) {
    if (txn == nullptr || txn->get_isolation_level() != IsolationLevel::SERIALIZABLE) {
        return;
    }
    std::lock_guard<std::mutex> lock(ssi_mutex_);
    txn_id_t writer = txn->get_transaction_id();
    WriteIntent intent{writer, fd, table_name, rid, old_record, new_record, txn->get_start_ts(),
                       INVALID_TS, false};
    serializable_write_intents_[writer].push_back(intent);
    serializable_write_intent_fd_index_[fd].insert(writer);
    TupleKey key{fd, rid};

    auto rid_readers_it = serializable_rid_read_index_.find(key);
    if (rid_readers_it != serializable_rid_read_index_.end()) {
        auto candidate_readers = rid_readers_it->second;
        for (txn_id_t reader : candidate_readers) {
            if (reader == writer) {
                continue;
            }
            if (AddSerializableRwEdgeAndCheck(reader, writer, writer)) {
                throw TransactionAbortException(writer, AbortReason::SSI_DANGEROUS_STRUCTURE);
            }
        }
    }

    auto predicate_readers_it = serializable_predicate_read_fd_index_.find(fd);
    if (predicate_readers_it == serializable_predicate_read_fd_index_.end()) {
        return;
    }
    auto candidate_predicate_readers = predicate_readers_it->second;
    for (txn_id_t reader : candidate_predicate_readers) {
        if (reader == writer) {
            continue;
        }
        auto predicate_entry = serializable_predicate_reads_.find(reader);
        if (predicate_entry == serializable_predicate_reads_.end()) {
            continue;
        }
        for (const auto &predicate : predicate_entry->second) {
            if (predicate.fd != fd) {
                continue;
            }
            if (SerializableWriteAffectsPredicate(predicate, intent) &&
                AddSerializableRwEdgeAndCheck(reader, writer, writer)) {
                throw TransactionAbortException(writer, AbortReason::SSI_DANGEROUS_STRUCTURE);
            }
        }
    }
}

void TransactionManager::RecordInsert(RmFileHandle *fh, const Rid &rid, Transaction *txn) {
    if (txn == nullptr) {
        return;
    }
    if (fh == nullptr) {
        throw InternalError("Insert version state requires a file handle");
    }
    fh->MarkHasMvccState();
    int fd = fh->GetFd();
    TupleKey key{fd, rid};
    auto &shard = ShardFor(key);
    std::unique_lock<std::shared_mutex> lock(shard.mutex);
    if (!txn->HasTupleWriteState(key)) {
        TxnTupleWriteState write_state;
        write_state.inserted_by_txn = true;
        write_state.has_before_state = false;
        txn->SetTupleWriteState(key, write_state);
    }
    AssignTupleStateLocked(shard, key, TupleState{INVALID_TS, txn->get_transaction_id(), false, std::nullopt});
}

void TransactionManager::RecordUpdate(RmFileHandle *fh, const Rid &rid, const RmRecord &old_record, Transaction *txn) {
    if (txn == nullptr) {
        return;
    }
    if (fh == nullptr) {
        throw InternalError("Update version state requires a file handle");
    }
    fh->MarkHasMvccState();
    int fd = fh->GetFd();
    TupleKey key{fd, rid};
    auto &shard = ShardFor(key);
    std::unique_lock<std::shared_mutex> lock(shard.mutex);
    auto existing = shard.states.find(key);
    TupleState before = existing == shard.states.end() ? TupleState{} : existing->second;
    bool first_write = !txn->HasTupleWriteState(key);
    if (first_write) {
        TxnTupleWriteState write_state;
        write_state.inserted_by_txn = false;
        write_state.has_before_state = true;
        if (before.owner == txn->get_transaction_id()) {
            before.owner = INVALID_TXN_ID;
        }
        write_state.before_first_write = before;

        UndoLog undo;
        undo.is_deleted_ = before.is_deleted;
        undo.record_ = old_record;
        undo.ts_ = before.commit_ts;
        if (before.undo_head.has_value()) {
            undo.prev_version_ = before.undo_head.value();
        }
        UndoLink undo_link = txn->AppendUndoLog(std::move(undo));
        before.undo_head = undo_link;
        txn->SetTupleWriteState(key, write_state);
    }
    before.owner = txn->get_transaction_id();
    before.is_deleted = false;
    AssignTupleStateLocked(shard, key, before);
}

void TransactionManager::RecordDelete(RmFileHandle *fh, const Rid &rid, const RmRecord &old_record, Transaction *txn) {
    if (txn == nullptr) {
        return;
    }
    if (fh == nullptr) {
        throw InternalError("Delete version state requires a file handle");
    }
    fh->MarkHasMvccState();
    int fd = fh->GetFd();
    TupleKey key{fd, rid};
    auto &shard = ShardFor(key);
    std::unique_lock<std::shared_mutex> lock(shard.mutex);
    auto existing = shard.states.find(key);
    TupleState before = existing == shard.states.end() ? TupleState{} : existing->second;
    bool first_write = !txn->HasTupleWriteState(key);
    if (first_write) {
        TxnTupleWriteState write_state;
        write_state.inserted_by_txn = false;
        write_state.has_before_state = true;
        if (before.owner == txn->get_transaction_id()) {
            before.owner = INVALID_TXN_ID;
        }
        write_state.before_first_write = before;

        UndoLog undo;
        undo.is_deleted_ = before.is_deleted;
        undo.record_ = old_record;
        undo.ts_ = before.commit_ts;
        if (before.undo_head.has_value()) {
            undo.prev_version_ = before.undo_head.value();
        }
        UndoLink undo_link = txn->AppendUndoLog(std::move(undo));
        before.undo_head = undo_link;
        txn->SetTupleWriteState(key, write_state);
    }
    before.owner = txn->get_transaction_id();
    before.is_deleted = true;
    AssignTupleStateLocked(shard, key, before);
}

void TransactionManager::RestoreTupleStateForStatement(const TupleKey &key, Transaction *txn) {
    if (txn == nullptr) {
        return;
    }
    auto state = txn->GetTupleWriteState(key);
    if (state == nullptr) {
        return;
    }
    auto &shard = ShardFor(key);
    std::unique_lock<std::shared_mutex> lock(shard.mutex);
    if (state->inserted_by_txn && !state->has_before_state) {
        EraseTupleStateLocked(shard, key);
    } else if (state->has_before_state) {
        AssignTupleStateLocked(shard, key, state->before_first_write);
    } else {
        EraseTupleStateLocked(shard, key);
    }
    txn->EraseTupleWriteState(key);
}

void TransactionManager::RestoreOwnedTupleFlags(const std::vector<OwnedTupleFlagChange> &changes,
                                                Transaction *txn) {
    if (txn == nullptr || changes.empty()) {
        return;
    }
    // 同一语句通常每个 RID 只出现一次；逆序恢复仍能覆盖未来可能出现的重复候选。
    for (auto it = changes.rbegin(); it != changes.rend(); ++it) {
        auto &shard = ShardFor(it->key);
        std::unique_lock<std::shared_mutex> lock(shard.mutex);
        auto state_it = shard.states.find(it->key);
        if (state_it != shard.states.end() && state_it->second.owner == txn->get_transaction_id()) {
            state_it->second.is_deleted = it->previous_is_deleted;
            shard.publication_epoch.fetch_add(1, std::memory_order_release);
        }
    }
}

void TransactionManager::RecoverySetTupleState(RmFileHandle *fh, const Rid &rid, timestamp_t commit_ts,
                                               bool is_deleted) {
    if (fh == nullptr) {
        throw InternalError("Recovery version state requires a file handle");
    }
    fh->MarkHasMvccState();
    int fd = fh->GetFd();
    TupleKey key{fd, rid};
    auto &shard = ShardFor(key);
    std::unique_lock<std::shared_mutex> lock(shard.mutex);
    AssignTupleStateLocked(shard, key, TupleState{commit_ts, INVALID_TXN_ID, is_deleted, std::nullopt});
}

void TransactionManager::VacuumCommittedDeletes() {
    // 仅在静态检查点的静默态（无其它活跃事务）下调用，因此不存在旧快照仍需读取
    // 被删除记录旧版本的情况，可以安全地把逻辑删除的 heap slot 物理释放。
    std::unordered_map<int, RmFileHandle *> fd_to_fh;
    for (auto &entry : sm_manager_->fhs_) {
        fd_to_fh[entry.second->GetFd()] = entry.second.get();
    }

    auto quarantine = [&](int fd, const Rid &rid) {
        auto fh_it = fd_to_fh.find(fd);
        if (fh_it != fd_to_fh.end()) {
            try {
                fh_it->second->delete_record_quarantine(rid);
            } catch (...) {
                // 物理清除失败不影响正确性：记录仍在堆中，最坏情况退化为未清除。
            }
        }
    };

    bool do_full_scan = false;
    std::unordered_set<TupleKey> pending_keys;
    {
        std::lock_guard<std::mutex> vacuum_lock(vacuum_mutex_);
        if (!vacuum_tracking_active_) {
            vacuum_tracking_active_ = true;
            pending_committed_deletes_.clear();
            do_full_scan = true;
        } else {
            pending_keys.swap(pending_committed_deletes_);
        }
    }

    if (do_full_scan) {
        // 首个检查点：全量扫描一次，清除本进程此前累积的全部已提交删除，
        // 然后开启增量跟踪，后续检查点不再做 O(全部 tuple_states_) 扫描。
        for (auto &shard : tuple_state_shards_) {
            std::unique_lock<std::shared_mutex> lock(shard.mutex);
            for (auto it = shard.states.begin(); it != shard.states.end();) {
                const TupleKey key = it->first;
                const TupleState &state = it->second;
                if (state.owner == INVALID_TXN_ID) {
                    if (state.is_deleted) {
                        quarantine(key.fd, key.rid);
                    }
                    it = shard.states.erase(it);
                    shard.approx_size.fetch_sub(1, std::memory_order_release);
                    shard.publication_epoch.fetch_add(1, std::memory_order_release);
                } else {
                    ++it;
                }
            }
        }
        return;
    }

    // 增量路径：只处理自上次检查点以来新提交的删除键。键可能因随后被重新插入
    // 而不再是已提交删除，逐个复核当前状态后再决定是否物理清除。
    std::array<std::vector<const TupleKey *>, kTupleStateShardCount> tuple_state_groups;
    for (const auto &key : pending_keys) {
        tuple_state_groups[TupleStateShardIndex(key)].push_back(&key);
    }
    for (size_t shard_idx = 0; shard_idx < kTupleStateShardCount; ++shard_idx) {
        if (tuple_state_groups[shard_idx].empty()) {
            continue;
        }
        auto &shard = tuple_state_shards_[shard_idx];
        std::unique_lock<std::shared_mutex> lock(shard.mutex);
        for (const TupleKey *key : tuple_state_groups[shard_idx]) {
            auto it = shard.states.find(*key);
            if (it == shard.states.end()) {
                continue;
            }
            if (it->second.is_deleted && it->second.owner == INVALID_TXN_ID) {
                quarantine(key->fd, key->rid);
                EraseTupleStateLocked(shard, *key);
            }
        }
    }
}

void TransactionManager::RecoveryAdvanceCounters(txn_id_t next_txn_id, timestamp_t next_timestamp,
                                                 timestamp_t last_commit_ts) {
    txn_id_t current_txn = next_txn_id_.load();
    while (current_txn < next_txn_id &&
           !next_txn_id_.compare_exchange_weak(current_txn, next_txn_id)) {
    }
    timestamp_t current_ts = next_timestamp_.load();
    while (current_ts < next_timestamp &&
           !next_timestamp_.compare_exchange_weak(current_ts, next_timestamp)) {
    }
    timestamp_t current_commit = last_commit_ts_.load();
    while (current_commit < last_commit_ts &&
           !last_commit_ts_.compare_exchange_weak(current_commit, last_commit_ts)) {
    }
}

bool TransactionManager::UpdateUndoLink(Rid rid, std::optional<UndoLink> prev_link,
                                        std::function<bool(std::optional<UndoLink>)> &&check) {
    std::function<bool(std::optional<VersionUndoLink>)> version_check = nullptr;
    if (check) {
        version_check = [check = std::move(check)](std::optional<VersionUndoLink> current) mutable {
            if (!current.has_value()) {
                return check(std::nullopt);
            }
            return check(current->prev_);
        };
    }
    return UpdateVersionLink(rid, VersionUndoLink::FromOptionalUndoLink(prev_link), std::move(version_check));
}

bool TransactionManager::UpdateVersionLink(Rid rid, std::optional<VersionUndoLink> prev_version,
                                           std::function<bool(std::optional<VersionUndoLink>)> &&check) {
    std::unique_lock<std::shared_mutex> version_lock(version_info_mutex_);
    auto &page_info = version_info_[rid.page_no];
    if (page_info == nullptr) {
        page_info = std::make_shared<PageVersionInfo>();
    }
    std::unique_lock<std::shared_mutex> page_lock(page_info->mutex_);
    std::optional<VersionUndoLink> current = std::nullopt;
    auto it = page_info->prev_version_.find(rid.slot_no);
    if (it != page_info->prev_version_.end()) {
        current = it->second;
    }
    if (check && !check(current)) {
        return false;
    }
    if (prev_version.has_value()) {
        page_info->prev_version_[rid.slot_no] = prev_version.value();
    } else {
        page_info->prev_version_.erase(rid.slot_no);
    }
    return true;
}

std::optional<UndoLink> TransactionManager::GetUndoLink(Rid rid) {
    auto version = GetVersionLink(rid);
    if (!version.has_value()) {
        return std::nullopt;
    }
    return version->prev_;
}

std::optional<VersionUndoLink> TransactionManager::GetVersionLink(Rid rid) {
    std::shared_lock<std::shared_mutex> version_lock(version_info_mutex_);
    auto page_it = version_info_.find(rid.page_no);
    if (page_it == version_info_.end()) {
        return std::nullopt;
    }
    std::shared_lock<std::shared_mutex> page_lock(page_it->second->mutex_);
    auto it = page_it->second->prev_version_.find(rid.slot_no);
    if (it == page_it->second->prev_version_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<UndoLog> TransactionManager::GetUndoLogOptional(UndoLink link) {
    auto *txn = FindTransaction(link.prev_txn_);
    if (txn == nullptr) {
        return std::nullopt;
    }
    if (link.prev_log_idx_ < 0 || static_cast<size_t>(link.prev_log_idx_) >= txn->GetUndoLogNum()) {
        return std::nullopt;
    }
    return txn->GetUndoLog(link.prev_log_idx_);
}

UndoLog TransactionManager::GetUndoLog(UndoLink link) {
    auto undo = GetUndoLogOptional(link);
    if (!undo.has_value()) {
        throw InternalError("Undo log not found");
    }
    return undo.value();
}

timestamp_t TransactionManager::GetWatermark() {
    return running_txns_.GetWatermark();
}

void TransactionManager::GarbageCollection() {
    GarbageCollectStaleIndex();
}

void TransactionManager::CommitStaleIndexEntries(Transaction *txn, timestamp_t commit_ts,
                                                 const std::vector<StaleIndexEntry> &entries,
                                                 const StaleIndexCommitGroups &groups) {
    if (txn == nullptr || entries.empty()) {
        return;
    }
    txn_id_t txn_id = txn->get_transaction_id();
    for (size_t bucket_idx = 0; bucket_idx < kStaleIndexBucketCount; ++bucket_idx) {
        if (groups[bucket_idx].empty()) {
            continue;
        }
        auto &bucket = stale_index_buckets_[bucket_idx];
        std::unique_lock<std::shared_mutex> lock(bucket.mutex);
        for (size_t entry_idx : groups[bucket_idx]) {
            const auto &entry = entries[entry_idx];
            auto update_entry = [&](std::vector<StaleIndexEntry> &candidates) {
                for (auto &candidate : candidates) {
                    if (candidate.creating_txn == txn_id && candidate.retire_ts == INVALID_TS &&
                        candidate.key == entry.key && candidate.tuple == entry.tuple) {
                        candidate.retire_ts = commit_ts;
                    }
                }
            };

            auto equal_it = bucket.equal_entries.find(entry.key);
            if (equal_it != bucket.equal_entries.end()) {
                update_entry(equal_it->second);
            }

            auto index_it = bucket.entries_by_index.find(StaleIndexRegistryId(entry.key.fd, entry.key.index_no));
            if (index_it != bucket.entries_by_index.end()) {
                auto key_it = index_it->second.find(entry.key.encoded_key);
                if (key_it != index_it->second.end()) {
                    update_entry(key_it->second);
                }
            }
        }
    }
    txn->ClearStaleIndexEntries();
}

void TransactionManager::CleanupStaleIndexAbort(Transaction *txn) {
    if (txn == nullptr) {
        return;
    }
    // 这里传入的就是事务完整 stale 集合；registry 删除后直接清空事务侧向量，
    // 避免再做 candidates × entries 的二次匹配形成大 UPDATE 回滚 O(n^2)。
    auto entries = txn->GetStaleIndexEntriesSnapshot();
    RemoveStaleIndexEntries(entries, nullptr);
    txn->ClearStaleIndexEntries();
}

void TransactionManager::GarbageCollectStaleIndex() {
    std::uint64_t active_mask = stale_index_active_mask_.load(std::memory_order_acquire);
    if (active_mask == 0) {
        return;
    }
    timestamp_t watermark = GetWatermark();
    while (active_mask != 0) {
        size_t bucket_index = static_cast<size_t>(__builtin_ctzll(active_mask));
        const std::uint64_t bucket_bit = std::uint64_t{1} << bucket_index;
        active_mask &= ~bucket_bit;
        auto &bucket = stale_index_buckets_[bucket_index];
        std::unique_lock<std::shared_mutex> lock(bucket.mutex);
        if (bucket.entry_count.load(std::memory_order_acquire) == 0) {
            if (bucket.equal_entries.empty() && bucket.entries_by_index.empty()) {
                stale_index_active_mask_.fetch_and(~bucket_bit, std::memory_order_release);
            }
            continue;
        }
        auto expired = [&](const StaleIndexEntry &entry) {
            return entry.retire_ts != INVALID_TS && entry.retire_ts <= watermark;
        };
        size_t removed_count = 0;
        for (auto equal_it = bucket.equal_entries.begin(); equal_it != bucket.equal_entries.end();) {
            auto &entries = equal_it->second;
            size_t old_size = entries.size();
            entries.erase(std::remove_if(entries.begin(), entries.end(), expired), entries.end());
            removed_count += old_size - entries.size();
            if (entries.empty()) {
                equal_it = bucket.equal_entries.erase(equal_it);
            } else {
                ++equal_it;
            }
        }
        for (auto index_it = bucket.entries_by_index.begin(); index_it != bucket.entries_by_index.end();) {
            auto &key_map = index_it->second;
            for (auto key_it = key_map.begin(); key_it != key_map.end();) {
                auto &entries = key_it->second;
                entries.erase(std::remove_if(entries.begin(), entries.end(), expired), entries.end());
                if (entries.empty()) {
                    key_it = key_map.erase(key_it);
                } else {
                    ++key_it;
                }
            }
            if (key_map.empty()) {
                index_it = bucket.entries_by_index.erase(index_it);
            } else {
                ++index_it;
            }
        }
        if (removed_count > 0) {
            size_t previous = bucket.entry_count.fetch_sub(removed_count, std::memory_order_acq_rel);
            if (previous < removed_count) {
                bucket.entry_count.fetch_add(removed_count, std::memory_order_relaxed);
                throw InternalError("Stale index entry count underflow");
            }
            const size_t new_count = previous - removed_count;
            if (new_count == 0 && bucket.equal_entries.empty() && bucket.entries_by_index.empty()) {
                stale_index_active_mask_.fetch_and(~bucket_bit, std::memory_order_release);
            }
        }
    }
}

size_t TransactionManager::UniqueKeyShardIndex(const UniqueKeyId &key) {
    return UniqueKeyShardIndex(key.fd, key.index_no, key.encoded_key.data(), key.encoded_key.size());
}

size_t TransactionManager::UniqueKeyShardIndex(int fd, int index_no, const char *encoded_key, size_t key_size) {
    // 进程内分片不写入磁盘，采用稳定的 FNV-1a 混合，允许探测前在不构造 string 的
    // 情况下定位 generation；UniqueKeyId 路径必须复用同一函数保证分片一致。
    std::uint64_t hash = 1469598103934665603ULL;
    auto mix_u32 = [&](std::uint32_t value) {
        for (size_t i = 0; i < sizeof(value); ++i) {
            hash ^= static_cast<unsigned char>((value >> (i * 8U)) & 0xffU);
            hash *= 1099511628211ULL;
        }
    };
    mix_u32(static_cast<std::uint32_t>(fd));
    mix_u32(static_cast<std::uint32_t>(index_no));
    for (size_t i = 0; i < key_size; ++i) {
        hash ^= static_cast<unsigned char>(encoded_key[i]);
        hash *= 1099511628211ULL;
    }
    return static_cast<size_t>(hash) & (kUniqueKeyShardCount - 1);
}

TransactionManager::UniqueKeyShard &TransactionManager::UniqueKeyShardFor(const UniqueKeyId &key) {
    return unique_key_shards_[UniqueKeyShardIndex(key)];
}

std::uint64_t TransactionManager::GetUniqueKeyGeneration(
    int fd, int index_no, const std::vector<char> &encoded_key) const {
    size_t shard_idx = UniqueKeyShardIndex(fd, index_no, encoded_key.data(), encoded_key.size());
    return unique_key_shards_[shard_idx].generation.load(std::memory_order_acquire);
}

void TransactionManager::CaptureUniqueKeyStatementChangeLocked(
    const UniqueKeyId &key, UniqueKeyShard &shard, Transaction *txn,
    std::vector<UniqueKeyChange> *statement_changes) {
    if (txn == nullptr || statement_changes == nullptr) {
        return;
    }
    auto duplicate = std::find_if(statement_changes->begin(), statement_changes->end(),
                                  [&](const UniqueKeyChange &change) { return change.key == key; });
    if (duplicate != statement_changes->end()) {
        return;
    }

    UniqueKeyChange change;
    change.key = key;
    auto entry_it = shard.entries.find(key);
    change.had_before_entry = entry_it != shard.entries.end();
    if (change.had_before_entry) {
        change.before_entry = entry_it->second;
    }
    auto txn_state = txn->GetUniqueKeyWriteStateCopy(key);
    change.had_before_txn_state = txn_state.has_value();
    if (txn_state.has_value()) {
        change.before_txn_state = txn_state.value();
    }
    statement_changes->push_back(std::move(change));
}

TxnUniqueKeyWriteState TransactionManager::GetOrCreateTxnUniqueStateLocked(
    const UniqueKeyId &key, const UniqueKeyEntry &entry, bool had_entry, Transaction *txn) {
    auto existing = txn->GetUniqueKeyWriteStateCopy(key);
    if (existing.has_value()) {
        return existing.value();
    }
    TxnUniqueKeyWriteState state;
    state.key = key;
    state.had_before_entry = had_entry;
    if (had_entry) {
        state.before_entry = entry;
    }
    return state;
}

bool TransactionManager::ReserveUniqueKey(int fd, int index_no, const std::vector<char> &encoded_key,
                                          const Rid &rid, Transaction *txn,
                                          const std::optional<Rid> &current_owner,
                                          std::uint64_t observed_generation,
                                          std::vector<UniqueKeyChange> *statement_changes) {
    if (txn == nullptr) {
        throw InternalError("Unique key reservation requires a transaction");
    }

    UniqueKeyId key{fd, index_no, std::string(encoded_key.data(), encoded_key.size())};
    TupleKey new_owner{fd, rid};
    txn_id_t txn_id = txn->get_transaction_id();

    auto &shard = UniqueKeyShardFor(key);
    std::lock_guard<std::mutex> lock(shard.mutex);
    if (shard.generation.load(std::memory_order_relaxed) != observed_generation) {
        return false;
    }
    CaptureUniqueKeyStatementChangeLocked(key, shard, txn, statement_changes);

    auto entry_it = shard.entries.find(key);
    bool had_entry = entry_it != shard.entries.end();
    UniqueKeyEntry entry = had_entry ? entry_it->second : UniqueKeyEntry{};

    if (!entry.committed_owner_tuple.has_value() && current_owner.has_value()) {
        entry.committed_owner_tuple = TupleKey{fd, current_owner.value()};
    }

    if (entry.reserved_by_txn.has_value() && entry.reserved_by_txn.value() != txn_id) {
        throw DuplicateKeyError();
    }
    if (entry.pending_delete_by_txn.has_value() && entry.pending_delete_by_txn.value() != txn_id) {
        // Another active transaction is deleting this logical key. Reusing it would
        // race with that uncommitted write; let rmdb.cpp release statement_gate
        // and retry the statement without refreshing the transaction snapshot.
        throw StatementConflictRetry(entry.pending_delete_by_txn.value());
    }
    if (entry.reserved_by_txn.has_value() && entry.reserved_by_txn.value() == txn_id &&
        !SameTupleKey(entry.committed_owner_tuple, new_owner) && !entry.pending_delete_by_txn.has_value()) {
        auto existing_state = txn->GetUniqueKeyWriteStateCopy(key);
        if (!existing_state.has_value() || !SameTupleKey(existing_state->new_committed_owner, new_owner)) {
            throw DuplicateKeyError();
        }
    }
    if (entry.committed_owner_tuple.has_value() && !SameTupleKey(entry.committed_owner_tuple, new_owner) &&
        entry.pending_delete_by_txn.value_or(INVALID_TXN_ID) != txn_id) {
        throw DuplicateKeyError();
    }

    TxnUniqueKeyWriteState state = GetOrCreateTxnUniqueStateLocked(key, entry, had_entry, txn);
    entry.reserved_by_txn = txn_id;
    state.reserved_by_this_txn = true;
    state.new_committed_owner = new_owner;

    shard.entries[key] = entry;
    if (shard.entries.size() == kUniqueRegistryCompactBucketThreshold) {
        unique_registry_compaction_needed_.store(true, std::memory_order_release);
    }
    txn->SetUniqueKeyWriteState(key, std::move(state));
    return true;
}

void TransactionManager::CheckPendingKeyDeleteConflict(int fd, int index_no, const std::vector<char> &encoded_key,
                                                       Transaction *txn) {
    if (txn == nullptr) {
        return;
    }

    UniqueKeyId key{fd, index_no, std::string(encoded_key.data(), encoded_key.size())};
    txn_id_t txn_id = txn->get_transaction_id();

    auto &shard = UniqueKeyShardFor(key);
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto entry_it = shard.entries.find(key);
    if (entry_it == shard.entries.end()) {
        return;
    }
    const auto &entry = entry_it->second;
    if (entry.pending_delete_by_txn.has_value() && entry.pending_delete_by_txn.value() != txn_id) {
        throw StatementConflictRetry(entry.pending_delete_by_txn.value());
    }
}

void TransactionManager::MarkUniqueKeyDeleted(int fd, int index_no, const std::vector<char> &encoded_key,
                                              const Rid &rid, Transaction *txn,
                                              std::vector<UniqueKeyChange> *statement_changes) {
    if (txn == nullptr) {
        throw InternalError("Unique key delete marker requires a transaction");
    }

    UniqueKeyId key{fd, index_no, std::string(encoded_key.data(), encoded_key.size())};
    TupleKey old_owner{fd, rid};
    txn_id_t txn_id = txn->get_transaction_id();

    auto &shard = UniqueKeyShardFor(key);
    std::lock_guard<std::mutex> lock(shard.mutex);
    CaptureUniqueKeyStatementChangeLocked(key, shard, txn, statement_changes);

    auto entry_it = shard.entries.find(key);
    bool had_entry = entry_it != shard.entries.end();
    UniqueKeyEntry entry = had_entry ? entry_it->second : UniqueKeyEntry{};

    if (!entry.committed_owner_tuple.has_value() && !entry.reserved_by_txn.has_value()) {
        entry.committed_owner_tuple = old_owner;
    }
    if (entry.reserved_by_txn.has_value() && entry.reserved_by_txn.value() != txn_id) {
        throw DuplicateKeyError();
    }
    if (entry.pending_delete_by_txn.has_value() && entry.pending_delete_by_txn.value() != txn_id) {
        throw DuplicateKeyError();
    }
    if (entry.committed_owner_tuple.has_value() && !SameTupleKey(entry.committed_owner_tuple, old_owner) &&
        entry.pending_delete_by_txn.value_or(INVALID_TXN_ID) != txn_id) {
        throw DuplicateKeyError();
    }

    TxnUniqueKeyWriteState state = GetOrCreateTxnUniqueStateLocked(key, entry, had_entry, txn);
    if (entry.reserved_by_txn.value_or(INVALID_TXN_ID) == txn_id &&
        !entry.committed_owner_tuple.has_value()) {
        state.new_committed_owner = std::nullopt;
    } else {
        entry.pending_delete_by_txn = txn_id;
        state.pending_delete_by_this_txn = true;
        state.deleted_committed_owner = entry.committed_owner_tuple.value_or(old_owner);
    }

    shard.entries[key] = entry;
    if (shard.entries.size() == kUniqueRegistryCompactBucketThreshold) {
        unique_registry_compaction_needed_.store(true, std::memory_order_release);
    }
    txn->SetUniqueKeyWriteState(key, std::move(state));
}

void TransactionManager::RestoreUniqueKeyChanges(const std::vector<UniqueKeyChange> &changes, Transaction *txn) {
    if (changes.empty()) {
        return;
    }
    std::array<bool, kUniqueKeyShardCount> needed{};
    for (const auto &change : changes) {
        needed[UniqueKeyShardIndex(change.key)] = true;
    }
    std::vector<std::unique_lock<std::mutex>> locks;
    locks.reserve(kUniqueKeyShardCount);
    for (size_t i = 0; i < kUniqueKeyShardCount; ++i) {
        if (needed[i]) {
            locks.emplace_back(unique_key_shards_[i].mutex);
        }
    }
    for (size_t i = 0; i < kUniqueKeyShardCount; ++i) {
        if (needed[i]) {
            unique_key_shards_[i].generation.fetch_add(1, std::memory_order_release);
        }
    }
    for (auto it = changes.rbegin(); it != changes.rend(); ++it) {
        auto &entries = UniqueKeyShardFor(it->key).entries;
        if (it->had_before_entry) {
            entries[it->key] = it->before_entry;
        } else {
            entries.erase(it->key);
        }
        if (txn != nullptr) {
            if (it->had_before_txn_state) {
                txn->SetUniqueKeyWriteState(it->key, it->before_txn_state);
            } else {
                txn->EraseUniqueKeyWriteState(it->key);
            }
        }
    }
}

void TransactionManager::PrepareUniqueKeyCommit(Transaction *txn,
                                                const std::vector<TxnUniqueKeyWriteState> &states,
                                                const UniqueKeyCommitGroups &groups) {
    if (txn == nullptr || states.empty()) {
        return;
    }
    txn_id_t txn_id = txn->get_transaction_id();
    for (size_t shard_idx = 0; shard_idx < kUniqueKeyShardCount; ++shard_idx) {
        if (groups[shard_idx].empty()) {
            continue;
        }
        auto &shard = unique_key_shards_[shard_idx];
        std::lock_guard<std::mutex> lock(shard.mutex);
        for (size_t state_idx : groups[shard_idx]) {
            const auto &state = states[state_idx];
            auto entry_it = shard.entries.find(state.key);
            if (entry_it == shard.entries.end()) {
                throw InternalError("Unique key state missing before commit");
            }
            auto &entry = entry_it->second;
            bool owns_expected_state = true;
            if (state.pending_delete_by_this_txn &&
                entry.pending_delete_by_txn.value_or(INVALID_TXN_ID) != txn_id) {
                owns_expected_state = false;
            }
            if (state.reserved_by_this_txn &&
                entry.reserved_by_txn.value_or(INVALID_TXN_ID) != txn_id) {
                owns_expected_state = false;
            }
            if (!owns_expected_state ||
                (!state.pending_delete_by_this_txn && !state.reserved_by_this_txn)) {
                throw InternalError("Unique key ownership changed before commit");
            }
        }
    }
}

void TransactionManager::CommitUniqueKeyEntries(Transaction *txn,
                                                const std::vector<TxnUniqueKeyWriteState> &states,
                                                const UniqueKeyCommitGroups &groups) {
    if (txn == nullptr || states.empty()) {
        return;
    }
    txn_id_t txn_id = txn->get_transaction_id();
    for (size_t shard_idx = 0; shard_idx < kUniqueKeyShardCount; ++shard_idx) {
        if (groups[shard_idx].empty()) {
            continue;
        }
        auto &shard = unique_key_shards_[shard_idx];
        std::lock_guard<std::mutex> lock(shard.mutex);
        // B+ 树物理变化已经完成；先使并发点查的 observation token 失效，再发布
        // transient registry 的最终状态。Reserve 在同一 shard 锁下验证该 token。
        shard.generation.fetch_add(1, std::memory_order_release);
        for (size_t state_idx : groups[shard_idx]) {
            const auto &state = states[state_idx];
            auto entry_it = shard.entries.find(state.key);
            if (entry_it == shard.entries.end()) {
                throw InternalError("Unique key state missing at commit publication");
            }
            auto &entry = entry_it->second;
            if (state.pending_delete_by_this_txn &&
                entry.pending_delete_by_txn.value_or(INVALID_TXN_ID) == txn_id) {
                entry.committed_owner_tuple = std::nullopt;
                entry.pending_delete_by_txn = std::nullopt;
            }
            if (state.reserved_by_this_txn &&
                entry.reserved_by_txn.value_or(INVALID_TXN_ID) == txn_id) {
                entry.reserved_by_txn = std::nullopt;
                entry.committed_owner_tuple = state.new_committed_owner;
            }

            // 稳定 owner 由唯一 B+ 树本身权威保存。注册表只协调活跃 reservation /
            // pending-delete，提交完成立即删除，避免 load 后按全量索引键线性占内存。
            if (!entry.reserved_by_txn.has_value() && !entry.pending_delete_by_txn.has_value()) {
                shard.entries.erase(entry_it);
            }
        }
    }
    txn->ClearUniqueKeyWriteStates();
}

void TransactionManager::CleanupUniqueKeyAbort(Transaction *txn) {
    if (txn == nullptr) {
        return;
    }
    auto states = txn->GetUniqueKeyWriteStatesSnapshot();
    if (states.empty()) {
        return;
    }

    std::array<bool, kUniqueKeyShardCount> needed{};
    for (const auto &state : states) {
        needed[UniqueKeyShardIndex(state.key)] = true;
    }
    std::vector<std::unique_lock<std::mutex>> locks;
    locks.reserve(kUniqueKeyShardCount);
    for (size_t i = 0; i < kUniqueKeyShardCount; ++i) {
        if (needed[i]) {
            locks.emplace_back(unique_key_shards_[i].mutex);
        }
    }
    for (size_t i = 0; i < kUniqueKeyShardCount; ++i) {
        if (needed[i]) {
            unique_key_shards_[i].generation.fetch_add(1, std::memory_order_release);
        }
    }
    for (const auto &state : states) {
        auto &entries = UniqueKeyShardFor(state.key).entries;
        if (state.had_before_entry) {
            entries[state.key] = state.before_entry;
        } else {
            entries.erase(state.key);
        }
    }
    txn->ClearUniqueKeyWriteStates();
}

void TransactionManager::MaybeCompactUniqueKeyRegistry() {
    if (!unique_registry_compaction_needed_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    bool retry_later = false;
    for (auto &shard : unique_key_shards_) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        if (!shard.entries.empty()) {
            retry_later = true;
            continue;
        }
        if (shard.entries.bucket_count() >= kUniqueRegistryCompactBucketThreshold) {
            try {
                shard.entries.rehash(0);
            } catch (...) {
                retry_later = true;
            }
        }
    }
    if (retry_later) {
        unique_registry_compaction_needed_.store(true, std::memory_order_release);
    }
}
