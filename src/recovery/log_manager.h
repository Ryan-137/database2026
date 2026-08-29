/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/config.h"
#include "log_defs.h"
#include "record/rm_defs.h"

/* 日志记录对应操作的类型 */
enum LogType : int {
    UPDATE = 0,
    INSERT,
    DELETE,
    begin,
    commit,
    ABORT,
    CHECKPOINT
};

class LogRecord {
public:
    LogType log_type_{LogType::begin};
    lsn_t lsn_{INVALID_LSN};
    uint32_t log_tot_len_{LOG_HEADER_SIZE};
    txn_id_t log_tid_{INVALID_TXN_ID};
    lsn_t prev_lsn_{INVALID_LSN};

    virtual ~LogRecord() = default;

    virtual void serialize(char *dest) const {
        memcpy(dest + OFFSET_LOG_TYPE, &log_type_, sizeof(LogType));
        memcpy(dest + OFFSET_LSN, &lsn_, sizeof(lsn_t));
        memcpy(dest + OFFSET_LOG_TOT_LEN, &log_tot_len_, sizeof(uint32_t));
        memcpy(dest + OFFSET_LOG_TID, &log_tid_, sizeof(txn_id_t));
        memcpy(dest + OFFSET_PREV_LSN, &prev_lsn_, sizeof(lsn_t));
    }

    virtual void deserialize(const char *src) {
        memcpy(&log_type_, src + OFFSET_LOG_TYPE, sizeof(LogType));
        memcpy(&lsn_, src + OFFSET_LSN, sizeof(lsn_t));
        memcpy(&log_tot_len_, src + OFFSET_LOG_TOT_LEN, sizeof(uint32_t));
        memcpy(&log_tid_, src + OFFSET_LOG_TID, sizeof(txn_id_t));
        memcpy(&prev_lsn_, src + OFFSET_PREV_LSN, sizeof(lsn_t));
    }

protected:
    static void WriteBytes(char *dest, int &offset, const void *src, size_t size) {
        memcpy(dest + offset, src, size);
        offset += static_cast<int>(size);
    }

    static void ReadBytes(const char *src, int &offset, void *dest, size_t size) {
        memcpy(dest, src + offset, size);
        offset += static_cast<int>(size);
    }

    static void WriteString(char *dest, int &offset, const std::string &value) {
        uint32_t size = static_cast<uint32_t>(value.size());
        WriteBytes(dest, offset, &size, sizeof(size));
        if (size > 0) {
            WriteBytes(dest, offset, value.data(), size);
        }
    }

    static std::string ReadString(const char *src, int &offset) {
        uint32_t size = 0;
        ReadBytes(src, offset, &size, sizeof(size));
        std::string value(src + offset, src + offset + size);
        offset += static_cast<int>(size);
        return value;
    }

    static void WriteRecord(char *dest, int &offset, const RmRecord &record) {
        WriteBytes(dest, offset, &record.size, sizeof(record.size));
        WriteBytes(dest, offset, record.data, record.size);
    }

    static RmRecord ReadRecord(const char *src, int &offset) {
        int size = 0;
        ReadBytes(src, offset, &size, sizeof(size));
        RmRecord record(size);
        memcpy(record.data, src + offset, size);
        offset += size;
        return record;
    }
};

class BeginLogRecord : public LogRecord {
public:
    BeginLogRecord() {
        log_type_ = LogType::begin;
        log_tot_len_ = LOG_HEADER_SIZE;
    }
    explicit BeginLogRecord(txn_id_t txn_id) : BeginLogRecord() { log_tid_ = txn_id; }
};

class CommitLogRecord : public LogRecord {
public:
    timestamp_t commit_ts_{INVALID_TS};

    CommitLogRecord() {
        log_type_ = LogType::commit;
        log_tot_len_ = LOG_HEADER_SIZE + sizeof(commit_ts_);
    }
    CommitLogRecord(txn_id_t txn_id, timestamp_t commit_ts) : CommitLogRecord() {
        log_tid_ = txn_id;
        commit_ts_ = commit_ts;
    }

    void serialize(char *dest) const override {
        LogRecord::serialize(dest);
        int offset = OFFSET_LOG_DATA;
        WriteBytes(dest, offset, &commit_ts_, sizeof(commit_ts_));
    }

    void deserialize(const char *src) override {
        LogRecord::deserialize(src);
        int offset = OFFSET_LOG_DATA;
        ReadBytes(src, offset, &commit_ts_, sizeof(commit_ts_));
    }
};

class AbortLogRecord : public LogRecord {
public:
    AbortLogRecord() {
        log_type_ = LogType::ABORT;
        log_tot_len_ = LOG_HEADER_SIZE;
    }
    explicit AbortLogRecord(txn_id_t txn_id) : AbortLogRecord() { log_tid_ = txn_id; }
};

class CheckpointLogRecord : public LogRecord {
public:
    txn_id_t next_txn_id_{0};
    timestamp_t next_timestamp_{1};
    timestamp_t last_commit_ts_{0};
    lsn_t durable_lsn_{0};

    CheckpointLogRecord() {
        log_type_ = LogType::CHECKPOINT;
        log_tot_len_ = LOG_HEADER_SIZE + sizeof(next_txn_id_) + sizeof(next_timestamp_) +
                       sizeof(last_commit_ts_) + sizeof(durable_lsn_);
    }
    CheckpointLogRecord(txn_id_t next_txn_id, timestamp_t next_timestamp,
                        timestamp_t last_commit_ts, lsn_t durable_lsn)
        : CheckpointLogRecord() {
        next_txn_id_ = next_txn_id;
        next_timestamp_ = next_timestamp;
        last_commit_ts_ = last_commit_ts;
        durable_lsn_ = durable_lsn;
    }

    void serialize(char *dest) const override {
        LogRecord::serialize(dest);
        int offset = OFFSET_LOG_DATA;
        WriteBytes(dest, offset, &next_txn_id_, sizeof(next_txn_id_));
        WriteBytes(dest, offset, &next_timestamp_, sizeof(next_timestamp_));
        WriteBytes(dest, offset, &last_commit_ts_, sizeof(last_commit_ts_));
        WriteBytes(dest, offset, &durable_lsn_, sizeof(durable_lsn_));
    }

    void deserialize(const char *src) override {
        LogRecord::deserialize(src);
        int offset = OFFSET_LOG_DATA;
        ReadBytes(src, offset, &next_txn_id_, sizeof(next_txn_id_));
        ReadBytes(src, offset, &next_timestamp_, sizeof(next_timestamp_));
        ReadBytes(src, offset, &last_commit_ts_, sizeof(last_commit_ts_));
        ReadBytes(src, offset, &durable_lsn_, sizeof(durable_lsn_));
    }
};

class InsertLogRecord : public LogRecord {
public:
    RmRecord insert_value_{};
    Rid rid_{};
    std::string table_name_;

    InsertLogRecord() {
        log_type_ = LogType::INSERT;
        log_tot_len_ = LOG_HEADER_SIZE;
    }
    InsertLogRecord(txn_id_t txn_id, const RmRecord &insert_value, const Rid &rid, std::string table_name)
        : InsertLogRecord() {
        log_tid_ = txn_id;
        insert_value_ = insert_value;
        rid_ = rid;
        table_name_ = std::move(table_name);
        log_tot_len_ += sizeof(int) + insert_value_.size + sizeof(Rid) + sizeof(uint32_t) + table_name_.size();
    }

    void serialize(char *dest) const override {
        LogRecord::serialize(dest);
        int offset = OFFSET_LOG_DATA;
        WriteRecord(dest, offset, insert_value_);
        WriteBytes(dest, offset, &rid_, sizeof(Rid));
        WriteString(dest, offset, table_name_);
    }

    void deserialize(const char *src) override {
        LogRecord::deserialize(src);
        int offset = OFFSET_LOG_DATA;
        insert_value_ = ReadRecord(src, offset);
        ReadBytes(src, offset, &rid_, sizeof(Rid));
        table_name_ = ReadString(src, offset);
    }
};

class DeleteLogRecord : public LogRecord {
public:
    RmRecord delete_value_{};
    Rid rid_{};
    std::string table_name_;

    DeleteLogRecord() {
        log_type_ = LogType::DELETE;
        log_tot_len_ = LOG_HEADER_SIZE;
    }
    DeleteLogRecord(txn_id_t txn_id, const RmRecord &delete_value, const Rid &rid, std::string table_name)
        : DeleteLogRecord() {
        log_tid_ = txn_id;
        delete_value_ = delete_value;
        rid_ = rid;
        table_name_ = std::move(table_name);
        log_tot_len_ += sizeof(int) + delete_value_.size + sizeof(Rid) + sizeof(uint32_t) + table_name_.size();
    }

    void serialize(char *dest) const override {
        LogRecord::serialize(dest);
        int offset = OFFSET_LOG_DATA;
        WriteRecord(dest, offset, delete_value_);
        WriteBytes(dest, offset, &rid_, sizeof(Rid));
        WriteString(dest, offset, table_name_);
    }

    void deserialize(const char *src) override {
        LogRecord::deserialize(src);
        int offset = OFFSET_LOG_DATA;
        delete_value_ = ReadRecord(src, offset);
        ReadBytes(src, offset, &rid_, sizeof(Rid));
        table_name_ = ReadString(src, offset);
    }
};

class UpdateLogRecord : public LogRecord {
public:
    RmRecord old_value_{};
    RmRecord new_value_{};
    Rid rid_{};
    std::string table_name_;

    UpdateLogRecord() {
        log_type_ = LogType::UPDATE;
        log_tot_len_ = LOG_HEADER_SIZE;
    }
    UpdateLogRecord(txn_id_t txn_id, const RmRecord &old_value, const RmRecord &new_value,
                    const Rid &rid, std::string table_name)
        : UpdateLogRecord() {
        log_tid_ = txn_id;
        old_value_ = old_value;
        new_value_ = new_value;
        rid_ = rid;
        table_name_ = std::move(table_name);
        log_tot_len_ += sizeof(int) + old_value_.size + sizeof(int) + new_value_.size + sizeof(Rid) +
                        sizeof(uint32_t) + table_name_.size();
    }

    void serialize(char *dest) const override {
        LogRecord::serialize(dest);
        int offset = OFFSET_LOG_DATA;
        WriteRecord(dest, offset, old_value_);
        WriteRecord(dest, offset, new_value_);
        WriteBytes(dest, offset, &rid_, sizeof(Rid));
        WriteString(dest, offset, table_name_);
    }

    void deserialize(const char *src) override {
        LogRecord::deserialize(src);
        int offset = OFFSET_LOG_DATA;
        old_value_ = ReadRecord(src, offset);
        new_value_ = ReadRecord(src, offset);
        ReadBytes(src, offset, &rid_, sizeof(Rid));
        table_name_ = ReadString(src, offset);
    }
};

std::unique_ptr<LogRecord> DeserializeLogRecord(const char *src, size_t size);

class LogBuffer {
public:
    LogBuffer() {
        offset_ = 0;
        base_offset_ = 0;
        memset(buffer_, 0, sizeof(buffer_));
    }

    void reset(lsn_t base_offset) {
        offset_ = 0;
        base_offset_ = base_offset;
    }

    bool is_full(int append_size) const { return offset_ + append_size > LOG_BUFFER_SIZE; }

    char buffer_[LOG_BUFFER_SIZE + 1];
    int offset_;
    lsn_t base_offset_;
};

class LogManager {
public:
    explicit LogManager(DiskManager *disk_manager) : disk_manager_(disk_manager) {}
    ~LogManager();

    void init_from_disk();
    void StartFlushThread();
    void StopFlushThread();
    lsn_t add_log_to_buffer(LogRecord *log_record);
    void flush_log_to_disk();
    void FlushUpTo(lsn_t record_lsn);
    // 在一次 latch 临界区中验证一批 page LSN，并把 WAL 持久化到其中
    // 最靠后的完整日志记录。任一未知 LSN 都会在写数据页前失败。
    void FlushPageLsns(const std::vector<lsn_t> &record_lsns);

    lsn_t get_persist_lsn() const { return persist_lsn_.load(); }
    lsn_t get_record_end_offset(lsn_t record_lsn);
    bool has_record_lsn(lsn_t record_lsn);
    lsn_t AppendCheckpointRecord(txn_id_t next_txn_id, timestamp_t next_timestamp,
                                 timestamp_t last_commit_ts, lsn_t redo_start_lsn);
    void WriteRestartFile(lsn_t checkpoint_lsn);
    void MarkDurablePrefix(lsn_t checkpoint_lsn);
    bool ReadRestartFile(lsn_t *checkpoint_lsn);
    LogBuffer *get_log_buffer() { return active_buffer_; }

private:
    void FlushTo(lsn_t exclusive_end_offset);
    void RequestFlushAndWait(lsn_t exclusive_end_offset);
    void FlushThreadMain();

    std::atomic<lsn_t> global_lsn_{0};
    std::atomic<lsn_t> persist_lsn_{0};
    std::mutex latch_;
    std::condition_variable flush_cv_;             // 唤醒后台线程交换并写出当前 active buffer
    std::condition_variable durable_cv_;           // 唤醒等待目标 LSN 持久化的事务线程
    std::condition_variable buffer_available_cv_;  // active buffer 满时，等待后台 swap 腾出空间
    LogBuffer log_buffers_[2];
    LogBuffer *active_buffer_{&log_buffers_[0]};   // 前台追加日志的缓冲区
    LogBuffer *flush_buffer_{&log_buffers_[1]};    // 后台写盘使用的缓冲区
    std::unordered_map<lsn_t, lsn_t> record_end_offsets_;
    // When a valid restart file lets us rebuild offsets from checkpoint_lsn,
    // records before this boundary are known durable but intentionally not indexed.
    lsn_t durable_prefix_lsn_{0};
    bool flush_thread_started_{false};
    bool stop_flush_thread_{false};
    std::thread flush_thread_;
    DiskManager *disk_manager_;
};
