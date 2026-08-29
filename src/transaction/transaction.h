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

#include <atomic>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/common.h"
#include "transaction/txn_defs.h"
#include "record/rm_defs.h"

/** 表示此tuple的前一个版本的链接 */
struct UndoLink {
  /* 之前的版本可以在其中的事务中找到 */
  txn_id_t prev_txn_{INVALID_TXN_ID};
  /* 在 `prev_txn_` 中前一个版本的日志索引 */
  int prev_log_idx_{0};

  friend auto operator==(const UndoLink &a, const UndoLink &b) {
    return a.prev_txn_ == b.prev_txn_ && a.prev_log_idx_ == b.prev_log_idx_;
  }

  friend auto operator!=(const UndoLink &a, const UndoLink &b) { return !(a == b); }

  /* Checks if the undo link points to something. */
  bool IsValid() { return prev_txn_ != INVALID_TXN_ID; }
};

struct UndoLog {
  bool is_deleted_{false};
  RmRecord record_{};
  timestamp_t ts_{INVALID_TS};
  UndoLink prev_version_{};
};

struct TupleState {
  timestamp_t commit_ts{0};
  txn_id_t owner{INVALID_TXN_ID};
  bool is_deleted{false};
  std::optional<UndoLink> undo_head{std::nullopt};
};

struct TxnTupleWriteState {
  bool inserted_by_txn{false};
  bool has_before_state{false};
  TupleState before_first_write{};
};

class Transaction {
   public:
    explicit Transaction(txn_id_t txn_id, IsolationLevel isolation_level = IsolationLevel::SNAPSHOT_ISOLATION)
        : state_(TransactionState::DEFAULT), isolation_level_(isolation_level), txn_id_(txn_id) {
        prev_lsn_ = INVALID_LSN;
        thread_id_ = std::this_thread::get_id();
    }

    ~Transaction() = default;

    inline txn_id_t get_transaction_id() { return txn_id_; }

    inline std::thread::id get_thread_id() { return thread_id_; }

    inline void set_txn_mode(bool txn_mode) { txn_mode_ = txn_mode; }
    inline bool get_txn_mode() { return txn_mode_; }

    inline void set_start_ts(timestamp_t start_ts) { start_ts_ = start_ts; }
    inline timestamp_t get_start_ts() { return start_ts_; }

    inline IsolationLevel get_isolation_level() { return isolation_level_; }
    inline void set_isolation_level(IsolationLevel isolation_level) { isolation_level_ = isolation_level; }

    inline TransactionState get_state() const { return state_.load(std::memory_order_acquire); }
    inline void set_state(TransactionState state) { state_.store(state, std::memory_order_release); }

    inline lsn_t get_prev_lsn() { return prev_lsn_; }
    inline void set_prev_lsn(lsn_t prev_lsn) { prev_lsn_ = prev_lsn; }

    inline std::deque<WriteRecord *> *get_write_set() { return &write_set_; }
    inline void append_write_record(WriteRecord* write_record) { write_set_.push_back(write_record); }

    inline std::deque<Page *> *get_index_deleted_page_set() { return &index_deleted_page_set_; }
    inline void append_index_deleted_page(Page* page) { index_deleted_page_set_.push_back(page); }

    inline std::deque<Page *> *get_index_latch_page_set() { return &index_latch_page_set_; }
    inline void append_index_latch_page_set(Page* page) { index_latch_page_set_.push_back(page); }

    inline std::unordered_set<LockDataId> *get_lock_set() { return &lock_set_; }

    inline timestamp_t get_read_ts() const { return read_ts_; }
    inline void set_read_ts(timestamp_t read_ts) { read_ts_ = read_ts; }
    inline timestamp_t get_commit_ts() const { return commit_ts_; }
    inline void set_commit_ts(timestamp_t commit_ts) { commit_ts_ = commit_ts; }

    /** 修改现有的撤销日志 */
    inline auto ModifyUndoLog(int log_idx, UndoLog new_log) {
        std::scoped_lock<std::mutex> lck(latch_);
        undo_logs_[log_idx] = std::move(new_log);
      }

    /** @return 此事务中撤销日志的索引 */
    inline auto AppendUndoLog(UndoLog log) -> UndoLink {
        std::scoped_lock<std::mutex> lck(latch_);
        undo_logs_.emplace_back(std::move(log));
        return {txn_id_, static_cast<int>(undo_logs_.size() - 1)};
      }
    inline auto GetUndoLog(size_t log_id) -> UndoLog {
        std::scoped_lock<std::mutex> lck(latch_);
        return undo_logs_[log_id];
      }

    /** @return 撤销日志的数量 */
    inline auto GetUndoLogNum() -> size_t {
        std::scoped_lock<std::mutex> lck(latch_);
        return undo_logs_.size();
      }

    inline bool HasTupleWriteState(const TupleKey &key) {
        std::scoped_lock<std::mutex> lck(latch_);
        return tuple_write_states_.find(key) != tuple_write_states_.end();
    }

    inline TxnTupleWriteState *GetTupleWriteState(const TupleKey &key) {
        std::scoped_lock<std::mutex> lck(latch_);
        auto it = tuple_write_states_.find(key);
        if (it == tuple_write_states_.end()) {
            return nullptr;
        }
        return &it->second;
    }

    inline void SetTupleWriteState(const TupleKey &key, TxnTupleWriteState state) {
        std::scoped_lock<std::mutex> lck(latch_);
        tuple_write_states_[key] = std::move(state);
    }

    inline void EraseTupleWriteState(const TupleKey &key) {
        std::scoped_lock<std::mutex> lck(latch_);
        tuple_write_states_.erase(key);
    }

    inline std::vector<std::pair<TupleKey, TxnTupleWriteState>> GetTupleWriteStatesSnapshot() {
        std::scoped_lock<std::mutex> lck(latch_);
        std::vector<std::pair<TupleKey, TxnTupleWriteState>> states;
        states.reserve(tuple_write_states_.size());
        for (auto &entry : tuple_write_states_) {
            states.push_back(entry);
        }
        return states;
    }

    inline void ClearTupleWriteStates() {
        std::scoped_lock<std::mutex> lck(latch_);
        tuple_write_states_.clear();
    }

    inline void SetTupleStateGcKeys(std::vector<TupleKey> keys) {
        std::scoped_lock<std::mutex> lck(latch_);
        tuple_state_gc_keys_ = std::move(keys);
    }

    inline std::vector<TupleKey> TakeTupleStateGcKeys() {
        std::scoped_lock<std::mutex> lck(latch_);
        return std::move(tuple_state_gc_keys_);
    }

    inline void AddStaleIndexEntry(StaleIndexEntry entry) {
        std::scoped_lock<std::mutex> lck(latch_);
        stale_index_entries_.push_back(std::move(entry));
    }

    inline std::vector<StaleIndexEntry> GetStaleIndexEntriesSnapshot() {
        std::scoped_lock<std::mutex> lck(latch_);
        return stale_index_entries_;
    }

    inline void RemoveStaleIndexEntries(const std::vector<StaleIndexEntry> &entries) {
        std::scoped_lock<std::mutex> lck(latch_);
        stale_index_entries_.erase(std::remove_if(stale_index_entries_.begin(), stale_index_entries_.end(),
            [&](const StaleIndexEntry &candidate) {
                return std::any_of(entries.begin(), entries.end(), [&](const StaleIndexEntry &entry) {
                    return candidate.key == entry.key && candidate.tuple == entry.tuple &&
                           candidate.creating_txn == entry.creating_txn;
                });
            }), stale_index_entries_.end());
    }

    inline void ClearStaleIndexEntries() {
        std::scoped_lock<std::mutex> lck(latch_);
        stale_index_entries_.clear();
    }

    inline std::optional<TxnUniqueKeyWriteState> GetUniqueKeyWriteStateCopy(const UniqueKeyId &key) {
        std::scoped_lock<std::mutex> lck(latch_);
        auto it = unique_key_write_states_.find(key);
        if (it == unique_key_write_states_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    inline void SetUniqueKeyWriteState(const UniqueKeyId &key, TxnUniqueKeyWriteState state) {
        std::scoped_lock<std::mutex> lck(latch_);
        unique_key_write_states_[key] = std::move(state);
    }

    inline void EraseUniqueKeyWriteState(const UniqueKeyId &key) {
        std::scoped_lock<std::mutex> lck(latch_);
        unique_key_write_states_.erase(key);
    }

    inline std::vector<TxnUniqueKeyWriteState> GetUniqueKeyWriteStatesSnapshot() {
        std::scoped_lock<std::mutex> lck(latch_);
        std::vector<TxnUniqueKeyWriteState> states;
        states.reserve(unique_key_write_states_.size());
        for (auto &entry : unique_key_write_states_) {
            states.push_back(entry.second);
        }
        return states;
    }

    inline void ClearUniqueKeyWriteStates() {
        std::scoped_lock<std::mutex> lck(latch_);
        unique_key_write_states_.clear();
    }

    inline void MarkExpectedExistingUniquePoint(const UniqueKeyId &key) {
        std::scoped_lock<std::mutex> lck(latch_);
        expected_existing_unique_points_.insert(key);
    }

    inline bool HasExpectedExistingUniquePoint(const UniqueKeyId &key) {
        std::scoped_lock<std::mutex> lck(latch_);
        return expected_existing_unique_points_.find(key) != expected_existing_unique_points_.end();
    }

    inline bool RemovedUniqueKeyInThisTxn(const UniqueKeyId &key) {
        std::scoped_lock<std::mutex> lck(latch_);
        auto it = unique_key_write_states_.find(key);
        if (it == unique_key_write_states_.end()) {
            return false;
        }
        const auto &state = it->second;
        return state.pending_delete_by_this_txn ||
               (state.reserved_by_this_txn && !state.new_committed_owner.has_value());
    }

   private:
    bool txn_mode_;                   // 用于标识当前事务为显式事务还是单条SQL语句的隐式事务
    std::atomic<TransactionState> state_;  // 跨会话冲突检查与提交发布都会读取，必须原子发布
    IsolationLevel isolation_level_;  // 事务的隔离级别，默认隔离级别为可串行化
    std::thread::id thread_id_;       // 当前事务对应的线程id
    lsn_t prev_lsn_;                  // 当前事务执行的最后一条操作对应的lsn，用于系统故障恢复
    txn_id_t txn_id_;                 // 事务的ID，唯一标识符
    timestamp_t start_ts_;            // 事务的开始时间戳

    std::deque<WriteRecord *> write_set_;            // 事务包含的所有写操作
    std::unordered_set<LockDataId> lock_set_;        // 事务申请的所有锁
    std::deque<Page *> index_latch_page_set_;        // 维护事务执行过程中加锁的索引页面
    std::deque<Page *> index_deleted_page_set_;      // 维护事务执行过程中删除的索引页面

  std::atomic<timestamp_t> read_ts_{0};
  /** 提交时间戳 */
  std::atomic<timestamp_t> commit_ts_{INVALID_TS};
  /**
  * @brief 存储撤销日志。
  * 其他撤销日志/表堆将存储 (txn_id, index) 对，因此只能向此vector中追加内容或就地更新内容，而不能删除任何内容。
  */
  std::vector<UndoLog> undo_logs_;
  std::unordered_map<TupleKey, TxnTupleWriteState> tuple_write_states_;
  // 按 tuple-state shard 排好序的写键；仅保留到 watermark 安全退休点。
  std::vector<TupleKey> tuple_state_gc_keys_;
  std::vector<StaleIndexEntry> stale_index_entries_;
  std::unordered_map<UniqueKeyId, TxnUniqueKeyWriteState> unique_key_write_states_;
  std::unordered_set<UniqueKeyId> expected_existing_unique_points_;
  /** 用于访问事务级撤销日志的锁。 */
  std::mutex latch_;
};
