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

#include <map>
#include <unordered_set>
#include <unordered_map>
#include "log_manager.h"
#include "storage/disk_manager.h"
#include "system/sm_manager.h"
#include "transaction/transaction_manager.h"

class RedoLogsInPage {
public:
    RedoLogsInPage() { table_file_ = nullptr; }
    RmFileHandle* table_file_;
    std::vector<lsn_t> redo_logs_;   // 在该page上需要redo的操作的lsn
};

class RecoveryManager {
public:
    RecoveryManager(DiskManager* disk_manager, BufferPoolManager* buffer_pool_manager, SmManager* sm_manager,
                    TransactionManager *txn_manager) {
        disk_manager_ = disk_manager;
        buffer_pool_manager_ = buffer_pool_manager;
        sm_manager_ = sm_manager;
        txn_manager_ = txn_manager;
    }

    void analyze();
    void redo();
    void undo();
private:
    // 恢复末尾按 rmdb-main 通过版语义，从堆表整体重建所有索引，
    // 不再信任崩溃前可能结构不一致的磁盘 B+ 树做增量重放。
    void RebuildIndexesFromHeap();
    // 把恢复后的所有堆文件、索引文件与元数据强制落盘。
    void FlushAllRecoveredFiles();

    std::vector<std::unique_ptr<LogRecord>> logs_;
    std::unordered_map<txn_id_t, std::vector<LogRecord *>> logs_by_txn_;
    std::unordered_map<txn_id_t, timestamp_t> committed_txns_;
    txn_id_t max_txn_id_{INVALID_TXN_ID};
    timestamp_t max_commit_ts_{0};
    timestamp_t max_next_timestamp_{1};
    lsn_t scan_start_lsn_{0};
    bool restart_checkpoint_valid_{false};
    LogBuffer buffer_;                                              // 读入日志
    DiskManager* disk_manager_;                                     // 用来读写文件
    BufferPoolManager* buffer_pool_manager_;                        // 对页面进行读写
    SmManager* sm_manager_;                                         // 访问数据库元数据
    TransactionManager *txn_manager_;
};
