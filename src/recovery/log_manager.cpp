/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "log_manager.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <sstream>
#include <unistd.h>

#include "errors.h"

namespace {

bool TryReadRestartFile(lsn_t *checkpoint_lsn) {
    if (checkpoint_lsn == nullptr) {
        return false;
    }
    std::ifstream ifs(RESTART_FILE_NAME);
    if (!ifs.is_open()) {
        return false;
    }
    std::string line;
    std::getline(ifs, line);
    const std::string prefix = "checkpoint_lsn=";
    std::string value = line.rfind(prefix, 0) == 0 ? line.substr(prefix.size()) : line;
    if (value.empty()) {
        return false;
    }
    for (char ch : value) {
        if (ch < '0' || ch > '9') {
            return false;
        }
    }
    try {
        long long parsed = std::stoll(value);
        if (parsed < 0 || parsed > std::numeric_limits<lsn_t>::max()) {
            return false;
        }
        *checkpoint_lsn = static_cast<lsn_t>(parsed);
    } catch (...) {
        return false;
    }
    return true;
}

}  // namespace

std::unique_ptr<LogRecord> DeserializeLogRecord(const char *src, size_t size) {
    if (size < LOG_HEADER_SIZE) {
        return nullptr;
    }
    LogType type;
    uint32_t total_len;
    memcpy(&type, src + OFFSET_LOG_TYPE, sizeof(LogType));
    memcpy(&total_len, src + OFFSET_LOG_TOT_LEN, sizeof(uint32_t));
    if (total_len < LOG_HEADER_SIZE || total_len > size) {
        return nullptr;
    }

    std::unique_ptr<LogRecord> record;
    switch (type) {
        case LogType::begin:
            record = std::make_unique<BeginLogRecord>();
            break;
        case LogType::commit:
            record = std::make_unique<CommitLogRecord>();
            break;
        case LogType::ABORT:
            record = std::make_unique<AbortLogRecord>();
            break;
        case LogType::CHECKPOINT:
            record = std::make_unique<CheckpointLogRecord>();
            break;
        case LogType::INSERT:
            record = std::make_unique<InsertLogRecord>();
            break;
        case LogType::DELETE:
            record = std::make_unique<DeleteLogRecord>();
            break;
        case LogType::UPDATE:
            record = std::make_unique<UpdateLogRecord>();
            break;
        default:
            return nullptr;
    }
    record->deserialize(src);
    return record;
}

void LogManager::init_from_disk() {
    std::scoped_lock lock{latch_};
    record_end_offsets_.clear();
    durable_prefix_lsn_ = 0;

    int file_size = disk_manager_->get_file_size(LOG_FILE_NAME);
    if (file_size < 0) {
        global_lsn_.store(0);
        persist_lsn_.store(0);
        log_buffers_[0].reset(0);
        log_buffers_[1].reset(0);
        active_buffer_ = &log_buffers_[0];
        flush_buffer_ = &log_buffers_[1];
        return;
    }

    int offset = 0;
    lsn_t restart_lsn = INVALID_LSN;
    if (TryReadRestartFile(&restart_lsn) && restart_lsn >= 0 && restart_lsn + LOG_HEADER_SIZE <= file_size) {
        std::vector<char> checkpoint_header(LOG_HEADER_SIZE);
        int header_read = disk_manager_->read_log(checkpoint_header.data(), LOG_HEADER_SIZE, restart_lsn);
        uint32_t checkpoint_len = 0;
        memcpy(&checkpoint_len, checkpoint_header.data() + OFFSET_LOG_TOT_LEN, sizeof(uint32_t));
        if (header_read == LOG_HEADER_SIZE && checkpoint_len >= LOG_HEADER_SIZE &&
            restart_lsn + static_cast<lsn_t>(checkpoint_len) <= file_size) {
            std::vector<char> raw(checkpoint_len);
            int bytes_read = disk_manager_->read_log(raw.data(), static_cast<int>(checkpoint_len), restart_lsn);
            auto checkpoint = DeserializeLogRecord(raw.data(), raw.size());
            if (bytes_read == static_cast<int>(checkpoint_len) && checkpoint != nullptr &&
                checkpoint->lsn_ == restart_lsn && checkpoint->log_type_ == LogType::CHECKPOINT) {
                offset = restart_lsn;
                durable_prefix_lsn_ = restart_lsn;
            }
        }
    }
    std::vector<char> header(LOG_HEADER_SIZE);
    while (offset + LOG_HEADER_SIZE <= file_size) {
        int header_read = disk_manager_->read_log(header.data(), LOG_HEADER_SIZE, offset);
        if (header_read != LOG_HEADER_SIZE) {
            break;
        }
        uint32_t total_len = 0;
        memcpy(&total_len, header.data() + OFFSET_LOG_TOT_LEN, sizeof(uint32_t));
        if (total_len < LOG_HEADER_SIZE || offset + static_cast<int>(total_len) > file_size) {
            break;
        }
        record_end_offsets_[offset] = offset + static_cast<lsn_t>(total_len);
        offset += static_cast<int>(total_len);
    }
    global_lsn_.store(offset);
    persist_lsn_.store(offset);
    log_buffers_[0].reset(offset);
    log_buffers_[1].reset(offset);
    active_buffer_ = &log_buffers_[0];
    flush_buffer_ = &log_buffers_[1];
}

LogManager::~LogManager() {
    StopFlushThread();
}

void LogManager::StartFlushThread() {
    std::scoped_lock lock{latch_};
    if (flush_thread_started_) {
        return;
    }
    stop_flush_thread_ = false;
    flush_thread_ = std::thread(&LogManager::FlushThreadMain, this);
    flush_thread_started_ = true;
}

void LogManager::StopFlushThread() {
    {
        std::scoped_lock lock{latch_};
        if (!flush_thread_started_) {
            return;
        }
        stop_flush_thread_ = true;
    }
    flush_cv_.notify_one();
    if (flush_thread_.joinable()) {
        flush_thread_.join();
    }
    {
        std::scoped_lock lock{latch_};
        flush_thread_started_ = false;
        stop_flush_thread_ = false;
    }
    durable_cv_.notify_all();
    buffer_available_cv_.notify_all();
}

lsn_t LogManager::add_log_to_buffer(LogRecord *log_record) {
    if (log_record == nullptr) {
        throw InternalError("LogManager::add_log_to_buffer null log record");
    }
    std::unique_lock<std::mutex> lock{latch_};
    if (log_record->log_tot_len_ > LOG_BUFFER_SIZE) {
        throw InternalError("Single log record exceeds log buffer size");
    }
    int append_size = static_cast<int>(log_record->log_tot_len_);
    while (active_buffer_->is_full(append_size)) {
        if (!flush_thread_started_) {
            FlushTo(global_lsn_.load());
        } else {
            flush_cv_.notify_one();
            buffer_available_cv_.wait(lock, [&]() { return !active_buffer_->is_full(append_size); });
        }
    }

    lsn_t start_lsn = global_lsn_.load();
    lsn_t end_offset = start_lsn + static_cast<lsn_t>(log_record->log_tot_len_);
    log_record->lsn_ = start_lsn;

    // 先完成唯一可能分配内存的 LSN 索引登记，再触碰可见 buffer offset。
    // 这样 bad_alloc 不会留下“COMMIT 字节已写入 buffer、global_lsn 尚未推进”的半发布记录。
    auto [end_it, inserted] = record_end_offsets_.emplace(start_lsn, end_offset);
    if (!inserted) {
        throw InternalError("Duplicate WAL record start LSN");
    }
    try {
        log_record->serialize(active_buffer_->buffer_ + active_buffer_->offset_);
    } catch (...) {
        record_end_offsets_.erase(end_it);
        throw;
    }
    active_buffer_->offset_ += append_size;
    global_lsn_.store(end_offset);
    return start_lsn;
}

void LogManager::FlushTo(lsn_t exclusive_end_offset) {
    if (exclusive_end_offset <= persist_lsn_.load()) {
        return;
    }
    lsn_t buffered_end = global_lsn_.load();
    if (exclusive_end_offset > buffered_end) {
        throw InternalError("LogManager flush target exceeds appended log end");
    }

    if (active_buffer_->offset_ > 0) {
        lsn_t batch_start = active_buffer_->base_offset_;
        lsn_t batch_end = batch_start + static_cast<lsn_t>(active_buffer_->offset_);
        // 同步回退只在恢复阶段或后台线程未启动时使用，仍保持日志偏移连续。
        disk_manager_->write_log(active_buffer_->buffer_, active_buffer_->offset_,
                                 static_cast<off_t>(batch_start));
        active_buffer_->reset(batch_end);
        flush_buffer_->reset(batch_end);
        persist_lsn_.store(batch_end);
    }
}

void LogManager::flush_log_to_disk() {
    RequestFlushAndWait(global_lsn_.load());
}

lsn_t LogManager::get_record_end_offset(lsn_t record_lsn) {
    std::scoped_lock lock{latch_};
    auto it = record_end_offsets_.find(record_lsn);
    if (it == record_end_offsets_.end()) {
        throw InternalError("Unknown WAL record lsn " + std::to_string(record_lsn));
    }
    return it->second;
}

bool LogManager::has_record_lsn(lsn_t record_lsn) {
    std::scoped_lock lock{latch_};
    if (record_lsn != INVALID_LSN && record_lsn < durable_prefix_lsn_) {
        return true;
    }
    return record_end_offsets_.find(record_lsn) != record_end_offsets_.end();
}

void LogManager::FlushUpTo(lsn_t record_lsn) {
    if (record_lsn == INVALID_LSN) {
        return;
    }
    lsn_t target_lsn;
    {
        std::scoped_lock lock{latch_};
        if (record_lsn < durable_prefix_lsn_) {
            return;
        }
        auto it = record_end_offsets_.find(record_lsn);
        if (it == record_end_offsets_.end()) {
            throw InternalError("Unknown WAL record lsn " + std::to_string(record_lsn));
        }
        target_lsn = it->second;
    }
    RequestFlushAndWait(target_lsn);
}

void LogManager::FlushPageLsns(const std::vector<lsn_t> &record_lsns) {
    lsn_t target_end = 0;
    {
        std::scoped_lock lock{latch_};
        for (lsn_t record_lsn : record_lsns) {
            if (record_lsn == INVALID_LSN) {
                continue;
            }
            if (record_lsn < durable_prefix_lsn_) {
                continue;
            }
            auto it = record_end_offsets_.find(record_lsn);
            if (it == record_end_offsets_.end()) {
                throw InternalError("Unknown WAL record lsn " + std::to_string(record_lsn));
            }
            target_end = std::max(target_end, it->second);
        }
    }
    if (target_end > 0) {
        RequestFlushAndWait(target_end);
    }
}

void LogManager::RequestFlushAndWait(lsn_t exclusive_end_offset) {
    std::unique_lock<std::mutex> lock{latch_};
    if (exclusive_end_offset <= persist_lsn_.load()) {
        return;
    }
    if (!flush_thread_started_) {
        FlushTo(exclusive_end_offset);
        durable_cv_.notify_all();
        buffer_available_cv_.notify_all();
        return;
    }
    flush_cv_.notify_one();
    durable_cv_.wait(lock, [&]() { return persist_lsn_.load() >= exclusive_end_offset; });
}

void LogManager::FlushThreadMain() {
    while (true) {
        lsn_t batch_start = 0;
        lsn_t batch_end = 0;
        int batch_size = 0;
        LogBuffer *batch_buffer = nullptr;

        {
            std::unique_lock<std::mutex> lock{latch_};
            flush_cv_.wait(lock, [&]() { return stop_flush_thread_ || active_buffer_->offset_ > 0; });
            if (active_buffer_->offset_ == 0) {
                if (stop_flush_thread_) {
                    break;
                }
                continue;
            }

            batch_start = active_buffer_->base_offset_;
            batch_size = active_buffer_->offset_;
            batch_end = batch_start + static_cast<lsn_t>(batch_size);
            std::swap(active_buffer_, flush_buffer_);
            batch_buffer = flush_buffer_;

            // 交换后前台立刻拿到空缓冲区，日志写入不再阻塞 append 路径。
            active_buffer_->reset(batch_end);
            buffer_available_cv_.notify_all();
        }

        disk_manager_->write_log(batch_buffer->buffer_, batch_size, static_cast<off_t>(batch_start));

        {
            std::scoped_lock lock{latch_};
            batch_buffer->reset(batch_end);
            persist_lsn_.store(batch_end);
        }
        durable_cv_.notify_all();
    }
}

lsn_t LogManager::AppendCheckpointRecord(txn_id_t next_txn_id, timestamp_t next_timestamp,
                                         timestamp_t last_commit_ts, lsn_t redo_start_lsn) {
    if (redo_start_lsn == INVALID_LSN) {
        redo_start_lsn = global_lsn_.load();
    }
    CheckpointLogRecord log_record(next_txn_id, next_timestamp, last_commit_ts, redo_start_lsn);
    lsn_t checkpoint_lsn = add_log_to_buffer(&log_record);
    FlushUpTo(checkpoint_lsn);
    return checkpoint_lsn;
}

void LogManager::WriteRestartFile(lsn_t checkpoint_lsn) {
    std::string tmp_name = RESTART_FILE_NAME + ".tmp";
    std::string payload = std::to_string(checkpoint_lsn) + "\n";

    int fd = open(tmp_name.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd == -1) {
        throw UnixError();
    }
    size_t total_written = 0;
    while (total_written < payload.size()) {
        ssize_t written = write(fd, payload.data() + total_written, payload.size() - total_written);
        if (written <= 0) {
            close(fd);
            throw UnixError();
        }
        total_written += static_cast<size_t>(written);
    }
    if (close(fd) == -1) {
        throw UnixError();
    }
    if (rename(tmp_name.c_str(), RESTART_FILE_NAME.c_str()) == -1) {
        throw UnixError();
    }
}

void LogManager::MarkDurablePrefix(lsn_t checkpoint_lsn) {
    std::scoped_lock lock{latch_};
    if (checkpoint_lsn != INVALID_LSN && checkpoint_lsn > durable_prefix_lsn_) {
        durable_prefix_lsn_ = checkpoint_lsn;
        // Remove entries that are now covered by durable_prefix_lsn_.
        // has_record_lsn() returns true for any lsn < durable_prefix_lsn_
        // without consulting the map, so these entries are redundant.
        // Trimming prevents unbounded map growth across a long-running workload.
        for (auto it = record_end_offsets_.begin(); it != record_end_offsets_.end(); ) {
            if (it->first < durable_prefix_lsn_) {
                it = record_end_offsets_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

bool LogManager::ReadRestartFile(lsn_t *checkpoint_lsn) {
    return TryReadRestartFile(checkpoint_lsn);
}
