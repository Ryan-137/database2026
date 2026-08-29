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
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "common/config.h"
#include "defs.h"
#include "record/rm_defs.h"

/* 标识事务状态 */
enum class TransactionState { DEFAULT, GROWING, SHRINKING, COMMITTED, ABORTED };

/* 系统的隔离级别；题目九只评测 SNAPSHOT ISOLATION 与 SERIALIZABLE */
enum class IsolationLevel { SNAPSHOT_ISOLATION, SERIALIZABLE };

struct TupleKey {
    int fd{INVALID_PAGE_ID};
    Rid rid{};

    friend bool operator==(const TupleKey &lhs, const TupleKey &rhs) {
        return lhs.fd == rhs.fd && lhs.rid == rhs.rid;
    }
};

/* 事务写操作类型，包括插入、删除、更新三种操作 */
enum class WType { INSERT_TUPLE = 0, DELETE_TUPLE, UPDATE_TUPLE};

enum class WriteCheckResult {
    OK,
    CONFLICT_WITH_ACTIVE_WRITER,
    CONFLICT_WITH_COMMITTED_VERSION,
    NOT_VISIBLE_OR_DELETED
};

enum class TupleWriteKind {
    UPDATE,
    DELETE
};

struct WriteCheckOutcome {
    WriteCheckResult result{WriteCheckResult::OK};
    bool newly_acquired{false};
    txn_id_t owner{INVALID_TXN_ID};
    bool newly_recorded_tuple_state{false};
    bool changed_owned_delete_flag{false};
    bool previous_owned_delete_flag{false};
};

// 语句级重试日志：仅记录事务已经持有写权时被本语句改动的逻辑删除位。
// 首次取得写权的状态仍由 TxnTupleWriteState/RestoreTupleStateForStatement 负责恢复。
struct OwnedTupleFlagChange {
    TupleKey key;
    bool previous_is_deleted{false};
};

struct StaleIndexKey {
    int fd{INVALID_PAGE_ID};
    int index_no{-1};
    std::string encoded_key;

    friend bool operator==(const StaleIndexKey &lhs, const StaleIndexKey &rhs) {
        return lhs.fd == rhs.fd && lhs.index_no == rhs.index_no && lhs.encoded_key == rhs.encoded_key;
    }
};

struct StaleIndexEntry {
    StaleIndexKey key;
    TupleKey tuple;
    txn_id_t creating_txn{INVALID_TXN_ID};
    timestamp_t retire_ts{INVALID_TS};
};

struct UniqueKeyId {
    int fd{INVALID_PAGE_ID};
    int index_no{-1};
    std::string encoded_key;

    friend bool operator==(const UniqueKeyId &lhs, const UniqueKeyId &rhs) {
        return lhs.fd == rhs.fd && lhs.index_no == rhs.index_no && lhs.encoded_key == rhs.encoded_key;
    }
};

struct UniqueKeyEntry {
    std::optional<TupleKey> committed_owner_tuple{std::nullopt};
    std::optional<txn_id_t> reserved_by_txn{std::nullopt};
    std::optional<txn_id_t> pending_delete_by_txn{std::nullopt};
};

struct TxnUniqueKeyWriteState {
    UniqueKeyId key;
    bool had_before_entry{false};
    UniqueKeyEntry before_entry{};
    std::optional<TupleKey> new_committed_owner{std::nullopt};
    std::optional<TupleKey> deleted_committed_owner{std::nullopt};
    bool reserved_by_this_txn{false};
    bool pending_delete_by_this_txn{false};
};

struct UniqueKeyChange {
    UniqueKeyId key;
    bool had_before_entry{false};
    UniqueKeyEntry before_entry{};
    bool had_before_txn_state{false};
    TxnUniqueKeyWriteState before_txn_state{};
};

/**
 * @brief 事务的写操作记录，用于事务的回滚
 * INSERT records the new tuple. DELETE records the old tuple. UPDATE records both.
 */
class WriteRecord {
   public:
    WriteRecord() = default;

    // constructor for insert operation
    WriteRecord(WType wtype, const std::string &tab_name, const Rid &rid)
        : wtype_(wtype), tab_name_(tab_name), rid_(rid) {}

    // constructor for insert/delete operation with the tuple image needed by rollback
    WriteRecord(WType wtype, const std::string &tab_name, const Rid &rid, const RmRecord &record)
        : wtype_(wtype), tab_name_(tab_name), rid_(rid) {
        if (wtype == WType::INSERT_TUPLE) {
            new_record_ = record;
        } else {
            old_record_ = record;
        }
    }

    // constructor for update operation
    WriteRecord(WType wtype, const std::string &tab_name, const Rid &rid, const RmRecord &old_record,
                const RmRecord &new_record)
        : wtype_(wtype), tab_name_(tab_name), rid_(rid), old_record_(old_record), new_record_(new_record) {}

    ~WriteRecord() = default;

    inline RmRecord &GetRecord() { return old_record_.has_value() ? old_record_.value() : new_record_.value(); }

    inline const RmRecord &GetOldRecord() const { return old_record_.value(); }

    inline const RmRecord &GetNewRecord() const { return new_record_.value(); }

    inline bool HasOldRecord() const { return old_record_.has_value(); }

    inline bool HasNewRecord() const { return new_record_.has_value(); }

    inline Rid &GetRid() { return rid_; }

    inline WType &GetWriteType() { return wtype_; }

    inline std::string &GetTableName() { return tab_name_; }

   private:
    WType wtype_;
    std::string tab_name_;
    Rid rid_;
    std::optional<RmRecord> old_record_;
    std::optional<RmRecord> new_record_;
};

/* 多粒度锁，加锁对象的类型，包括记录和表 */
enum class LockDataType { TABLE = 0, RECORD = 1 };

/**
 * @description: 加锁对象的唯一标识
 */
class LockDataId {
   public:
    /* 表级锁 */
    LockDataId(int fd, LockDataType type) {
        assert(type == LockDataType::TABLE);
        fd_ = fd;
        type_ = type;
        rid_.page_no = -1;
        rid_.slot_no = -1;
    }

    /* 行级锁 */
    LockDataId(int fd, const Rid &rid, LockDataType type) {
        assert(type == LockDataType::RECORD);
        fd_ = fd;
        rid_ = rid;
        type_ = type;
    }

    inline int64_t Get() const {
        if (type_ == LockDataType::TABLE) {
            // fd_
            return static_cast<int64_t>(fd_);
        } else {
            // fd_, rid_.page_no, rid.slot_no
            return ((static_cast<int64_t>(type_)) << 63) | ((static_cast<int64_t>(fd_)) << 31) |
                   ((static_cast<int64_t>(rid_.page_no)) << 16) | rid_.slot_no;
        }
    }

    bool operator==(const LockDataId &other) const {
        if (type_ != other.type_) return false;
        if (fd_ != other.fd_) return false;
        return rid_ == other.rid_;
    }
    int fd_;
    Rid rid_;
    LockDataType type_;
};

template <>
struct std::hash<LockDataId> {
    size_t operator()(const LockDataId &obj) const { return std::hash<int64_t>()(obj.Get()); }
};

/* 事务回滚原因 */
enum class AbortReason {
    LOCK_ON_SHIRINKING = 0,
    UPGRADE_CONFLICT,
    DEADLOCK_PREVENTION,
    WRITE_WRITE_CONFLICT,
    SSI_DANGEROUS_STRUCTURE
};

/* 事务回滚异常，在rmdb.cpp中进行处理 */
class TransactionAbortException : public std::exception {
    txn_id_t txn_id_;
    AbortReason abort_reason_;

   public:
    explicit TransactionAbortException(txn_id_t txn_id, AbortReason abort_reason)
        : txn_id_(txn_id), abort_reason_(abort_reason) {}

    txn_id_t get_transaction_id() { return txn_id_; }
    AbortReason GetAbortReason() { return abort_reason_; }
    std::string GetInfo() {
        switch (abort_reason_) {
            case AbortReason::LOCK_ON_SHIRINKING: {
                return "Transaction " + std::to_string(txn_id_) +
                       " aborted because it cannot request locks on SHRINKING phase\n";
            } break;

            case AbortReason::UPGRADE_CONFLICT: {
                return "Transaction " + std::to_string(txn_id_) +
                       " aborted because another transaction is waiting for upgrading\n";
            } break;

            case AbortReason::DEADLOCK_PREVENTION: {
                return "Transaction " + std::to_string(txn_id_) + " aborted for deadlock prevention\n";
            } break;

            default: {
                return "Transaction aborted\n";
            } break;
        }
    }
};

// Statement-level conflict with another active writer. The transaction is not
// aborted yet; rmdb.cpp may release statement_gate and retry the whole SQL.
class StatementConflictRetry : public std::exception {
   public:
    explicit StatementConflictRetry(txn_id_t owner) : owner_(owner) {}
    txn_id_t owner() const { return owner_; }
    const char *what() const noexcept override { return "statement conflict retry"; }

   private:
    txn_id_t owner_;
};

template <>
struct std::hash<TupleKey> {
    size_t operator()(const TupleKey &key) const {
        size_t h1 = std::hash<int>()(key.fd);
        size_t h2 = std::hash<int>()(key.rid.page_no);
        size_t h3 = std::hash<int>()(key.rid.slot_no);
        return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
};

template <>
struct std::hash<StaleIndexKey> {
    size_t operator()(const StaleIndexKey &key) const {
        size_t h1 = std::hash<int>()(key.fd);
        size_t h2 = std::hash<int>()(key.index_no);
        size_t h3 = std::hash<std::string>()(key.encoded_key);
        return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
};

template <>
struct std::hash<UniqueKeyId> {
    size_t operator()(const UniqueKeyId &key) const {
        size_t h1 = std::hash<int>()(key.fd);
        size_t h2 = std::hash<int>()(key.index_no);
        size_t h3 = std::hash<std::string>()(key.encoded_key);
        return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
};
