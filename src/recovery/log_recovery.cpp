/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#include "log_recovery.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

#include "record/rm_scan.h"

namespace {

bool IsDml(LogType type) {
    return type == LogType::INSERT || type == LogType::DELETE || type == LogType::UPDATE;
}

bool HeapRidExists(RmFileHandle *fh, const Rid &rid) {
    if (fh == nullptr || rid.page_no < 0 || rid.page_no >= fh->get_file_hdr().num_pages) {
        return false;
    }
    return fh->is_record(rid);
}

bool ReadRestartCheckpoint(lsn_t *checkpoint_lsn) {
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
        *checkpoint_lsn = static_cast<lsn_t>(std::stoll(value));
    } catch (...) {
        return false;
    }
    return *checkpoint_lsn >= 0;
}

}  // namespace

/**
 * @description: analyze阶段，从最近检查点（或日志头）顺序读取日志，建立 committed/loser 事务集合。
 */
void RecoveryManager::analyze() {
    logs_.clear();
    logs_by_txn_.clear();
    committed_txns_.clear();
    max_txn_id_ = INVALID_TXN_ID;
    max_commit_ts_ = 0;
    max_next_timestamp_ = 1;
    scan_start_lsn_ = 0;
    restart_checkpoint_valid_ = false;

    int file_size = disk_manager_->get_file_size(LOG_FILE_NAME);
    if (file_size <= 0) {
        return;
    }

    lsn_t restart_lsn = INVALID_LSN;
    if (ReadRestartCheckpoint(&restart_lsn) && restart_lsn >= 0 && restart_lsn + LOG_HEADER_SIZE <= file_size) {
        char header[LOG_HEADER_SIZE];
        int header_read = disk_manager_->read_log(header, LOG_HEADER_SIZE, restart_lsn);
        uint32_t total_len = 0;
        memcpy(&total_len, header + OFFSET_LOG_TOT_LEN, sizeof(uint32_t));
        if (header_read == LOG_HEADER_SIZE && total_len >= LOG_HEADER_SIZE &&
            restart_lsn + static_cast<lsn_t>(total_len) <= file_size) {
            std::vector<char> raw(total_len);
            int bytes_read = disk_manager_->read_log(raw.data(), static_cast<int>(total_len), restart_lsn);
            auto checkpoint = DeserializeLogRecord(raw.data(), raw.size());
            if (bytes_read == static_cast<int>(total_len) && checkpoint != nullptr &&
                checkpoint->lsn_ == restart_lsn && checkpoint->log_type_ == LogType::CHECKPOINT) {
                auto *checkpoint_record = static_cast<CheckpointLogRecord *>(checkpoint.get());
                scan_start_lsn_ = checkpoint_record->durable_lsn_;
                if (scan_start_lsn_ < 0 || scan_start_lsn_ > restart_lsn ||
                    scan_start_lsn_ + LOG_HEADER_SIZE > file_size) {
                    scan_start_lsn_ = restart_lsn;
                }
                restart_checkpoint_valid_ = true;
            }
        }
    }

    int offset = scan_start_lsn_;
    while (offset + LOG_HEADER_SIZE <= file_size) {
        char header[LOG_HEADER_SIZE];
        int header_read = disk_manager_->read_log(header, LOG_HEADER_SIZE, offset);
        if (header_read != LOG_HEADER_SIZE) {
            break;
        }
        uint32_t total_len = 0;
        memcpy(&total_len, header + OFFSET_LOG_TOT_LEN, sizeof(uint32_t));
        if (total_len < LOG_HEADER_SIZE || offset + static_cast<int>(total_len) > file_size) {
            break;
        }

        std::vector<char> raw(total_len);
        int bytes_read = disk_manager_->read_log(raw.data(), static_cast<int>(total_len), offset);
        if (bytes_read != static_cast<int>(total_len)) {
            break;
        }
        auto record = DeserializeLogRecord(raw.data(), raw.size());
        if (record == nullptr || record->lsn_ != offset) {
            break;
        }

        max_txn_id_ = std::max(max_txn_id_, record->log_tid_);
        if (record->log_type_ == LogType::CHECKPOINT) {
            auto *checkpoint_record = static_cast<CheckpointLogRecord *>(record.get());
            max_txn_id_ = std::max(max_txn_id_, checkpoint_record->next_txn_id_ - 1);
            max_commit_ts_ = std::max(max_commit_ts_, checkpoint_record->last_commit_ts_);
            max_next_timestamp_ = std::max(max_next_timestamp_, checkpoint_record->next_timestamp_);
        } else if (record->log_type_ == LogType::commit) {
            auto *commit_record = static_cast<CommitLogRecord *>(record.get());
            committed_txns_[record->log_tid_] = commit_record->commit_ts_;
            max_commit_ts_ = std::max(max_commit_ts_, commit_record->commit_ts_);
            max_next_timestamp_ = std::max(max_next_timestamp_, commit_record->commit_ts_ + 1);
        }
        if (record->log_tid_ != INVALID_TXN_ID) {
            logs_by_txn_[record->log_tid_].push_back(record.get());
        }
        logs_.push_back(std::move(record));
        offset += static_cast<int>(total_len);
    }
}

/**
 * @description: 重做所有已提交事务的 heap DML。索引在 redo/undo 完成后由
 *               RebuildIndexesFromHeap 统一从堆表重建，此处不触碰任何索引，
 *               也不重建 MVCC tuple state（恢复后堆中只保留存活记录，默认可见）。
 */
void RecoveryManager::redo() {
    for (const auto &record_ptr : logs_) {
        LogRecord *record = record_ptr.get();
        if (!IsDml(record->log_type_) || committed_txns_.count(record->log_tid_) == 0) {
            continue;
        }

        if (record->log_type_ == LogType::INSERT) {
            auto *insert_record = static_cast<InsertLogRecord *>(record);
            auto file_it = sm_manager_->fhs_.find(insert_record->table_name_);
            if (file_it == sm_manager_->fhs_.end()) {
                continue;
            }
            auto *fh = file_it->second.get();
            if (HeapRidExists(fh, insert_record->rid_)) {
                fh->update_record(insert_record->rid_, insert_record->insert_value_.data, nullptr, "",
                                  nullptr, insert_record->lsn_);
            } else {
                fh->insert_record(insert_record->rid_, insert_record->insert_value_.data, insert_record->lsn_);
            }
        } else if (record->log_type_ == LogType::UPDATE) {
            auto *update_record = static_cast<UpdateLogRecord *>(record);
            auto file_it = sm_manager_->fhs_.find(update_record->table_name_);
            if (file_it == sm_manager_->fhs_.end()) {
                continue;
            }
            auto *fh = file_it->second.get();
            if (HeapRidExists(fh, update_record->rid_)) {
                fh->update_record(update_record->rid_, update_record->new_value_.data, nullptr, "",
                                  nullptr, update_record->lsn_);
            } else {
                fh->insert_record(update_record->rid_, update_record->new_value_.data, update_record->lsn_);
            }
        } else if (record->log_type_ == LogType::DELETE) {
            auto *delete_record = static_cast<DeleteLogRecord *>(record);
            auto file_it = sm_manager_->fhs_.find(delete_record->table_name_);
            if (file_it == sm_manager_->fhs_.end()) {
                continue;
            }
            // 已提交的 DELETE 直接把 heap slot 物理释放，使恢复后堆中只保留存活记录，
            // 与 rmdb-main 的物理删除语义一致，索引重建时不会把删除记录重新写回。
            file_it->second->delete_record_quarantine(delete_record->rid_, delete_record->lsn_);
        }
    }
}

/**
 * @description: 反向撤销未提交（loser）事务的 heap 修改。静态检查点保证发布时无活跃事务，
 *               因此 loser 事务必然完整出现在扫描区间内，heap undo 即可完全回滚其效果。
 */
void RecoveryManager::undo() {
    auto tuple_key = [&](LogRecord *record) -> std::optional<TupleKey> {
        std::string table_name;
        Rid rid{};
        if (record->log_type_ == LogType::INSERT) {
            auto *dml = static_cast<InsertLogRecord *>(record);
            table_name = dml->table_name_;
            rid = dml->rid_;
        } else if (record->log_type_ == LogType::UPDATE) {
            auto *dml = static_cast<UpdateLogRecord *>(record);
            table_name = dml->table_name_;
            rid = dml->rid_;
        } else if (record->log_type_ == LogType::DELETE) {
            auto *dml = static_cast<DeleteLogRecord *>(record);
            table_name = dml->table_name_;
            rid = dml->rid_;
        } else {
            return std::nullopt;
        }
        auto file_it = sm_manager_->fhs_.find(table_name);
        if (file_it == sm_manager_->fhs_.end()) return std::nullopt;
        return TupleKey{file_it->second->GetFd(), rid};
    };

    // Redo has already installed every committed image.  Undo loser records in
    // global reverse-LSN order, but never overwrite a newer committed image of
    // the same tuple.  Grouping by transaction (unordered-map iteration) can
    // otherwise replay an old aborted value over a later committed write.
    std::unordered_map<TupleKey, lsn_t> latest_committed_write;
    for (const auto &record_ptr : logs_) {
        LogRecord *record = record_ptr.get();
        if (!IsDml(record->log_type_) || committed_txns_.count(record->log_tid_) == 0) continue;
        auto key = tuple_key(record);
        if (key.has_value()) latest_committed_write[key.value()] = record->lsn_;
    }

    for (auto it = logs_.rbegin(); it != logs_.rend(); ++it) {
        LogRecord *record = it->get();
        if (!IsDml(record->log_type_) || committed_txns_.count(record->log_tid_) > 0) continue;
        auto key = tuple_key(record);
        if (!key.has_value()) continue;
        auto committed = latest_committed_write.find(key.value());
        if (committed != latest_committed_write.end() && committed->second > record->lsn_) continue;
        if (record->log_type_ == LogType::INSERT) {
            auto *insert_record = static_cast<InsertLogRecord *>(record);
            auto file_it = sm_manager_->fhs_.find(insert_record->table_name_);
            if (file_it == sm_manager_->fhs_.end()) {
                continue;
            }
            file_it->second->delete_record_quarantine(insert_record->rid_, insert_record->lsn_);
        } else if (record->log_type_ == LogType::UPDATE) {
            auto *update_record = static_cast<UpdateLogRecord *>(record);
            auto file_it = sm_manager_->fhs_.find(update_record->table_name_);
            if (file_it == sm_manager_->fhs_.end()) {
                continue;
            }
            auto *fh = file_it->second.get();
            if (HeapRidExists(fh, update_record->rid_)) {
                fh->update_record(update_record->rid_, update_record->old_value_.data, nullptr, "",
                                  nullptr, update_record->lsn_);
            } else {
                fh->insert_record(update_record->rid_, update_record->old_value_.data, update_record->lsn_);
            }
        } else if (record->log_type_ == LogType::DELETE) {
            auto *delete_record = static_cast<DeleteLogRecord *>(record);
            auto file_it = sm_manager_->fhs_.find(delete_record->table_name_);
            if (file_it == sm_manager_->fhs_.end()) {
                continue;
            }
            auto *fh = file_it->second.get();
            if (HeapRidExists(fh, delete_record->rid_)) {
                fh->update_record(delete_record->rid_, delete_record->delete_value_.data, nullptr, "",
                                  nullptr, delete_record->lsn_);
            } else {
                fh->insert_record(delete_record->rid_, delete_record->delete_value_.data, delete_record->lsn_);
            }
        }
    }

    txn_manager_->RecoveryAdvanceCounters(max_txn_id_ + 1, std::max(max_next_timestamp_, max_commit_ts_ + 1),
                                          max_commit_ts_);
    RebuildIndexesFromHeap();
    FlushAllRecoveredFiles();
}

/**
 * @description: 恢复末尾整体重建全部索引。heap redo/undo 结束后堆已一致且只含存活记录，
 *               逐表清空索引后按堆内容重新灌入，避免信任崩溃前可能结构不一致的磁盘 B+ 树。
 */
void RecoveryManager::RebuildIndexesFromHeap() {
    auto *ix_manager = sm_manager_->get_ix_manager();
    for (auto &fh_entry : sm_manager_->fhs_) {
        const std::string &table_name = fh_entry.first;
        RmFileHandle *fh = fh_entry.second.get();
        if (!sm_manager_->db_.is_table(table_name)) {
            continue;
        }
        TabMeta &tab = sm_manager_->db_.get_table(table_name);
        for (auto &index : tab.indexes) {
            std::string index_name = ix_manager->get_index_name(tab.name, index.cols);
            auto ih_it = sm_manager_->ihs_.find(index_name);
            if (ih_it == sm_manager_->ihs_.end()) {
                continue;
            }
            IxIndexHandle *ih = ih_it->second.get();
            ih->reset_to_empty();
            std::vector<char> key(index.col_tot_len);
            for (RmScan scan(fh); !scan.is_end(); scan.next()) {
                Rid rid = scan.rid();
                auto record = fh->get_record(rid, nullptr);
                int offset = 0;
                for (auto &col : index.cols) {
                    memcpy(key.data() + offset, record->data + col.offset, col.len);
                    offset += col.len;
                }
                ih->insert_entry(key.data(), rid, nullptr, INVALID_LSN, false);
            }
        }
    }
}

/**
 * @description: 把恢复后的堆文件、索引文件与数据库元数据强制落盘，保证恢复结果持久化。
 */
void RecoveryManager::FlushAllRecoveredFiles() {
    for (auto &entry : sm_manager_->fhs_) {
        entry.second->flush_file_hdr();
        buffer_pool_manager_->flush_all_pages(entry.second->GetFd());
    }
    for (auto &entry : sm_manager_->ihs_) {
        entry.second->flush_file_hdr();
        buffer_pool_manager_->flush_all_pages(entry.second->GetFd());
    }
    sm_manager_->flush_meta();
}
