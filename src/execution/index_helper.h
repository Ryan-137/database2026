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

#include <cstring>
#include <string>
#include <vector>

#include "errors.h"
#include "record/rm_defs.h"
#include "system/sm.h"
#include "transaction/transaction.h"

namespace IndexHelper {

constexpr int kImplicitLogicalKeyIndexNo = -10001;

inline std::vector<char> MakeKey(const IndexMeta &index, const RmRecord &record) {
    std::vector<char> key(index.col_tot_len);
    int offset = 0;
    for (int i = 0; i < index.col_num; ++i) {
        memcpy(key.data() + offset, record.data + index.cols[i].offset, index.cols[i].len);
        offset += index.cols[i].len;
    }
    return key;
}

inline std::vector<char> MakeImplicitLogicalKey(const TabMeta &table, const RmRecord &record) {
    if (table.cols.empty()) {
        return {};
    }
    const auto &col = table.cols.front();
    std::vector<char> key(col.len);
    memcpy(key.data(), record.data + col.offset, col.len);
    return key;
}

inline void InsertAll(SmManager *sm_manager, const std::string &tab_name, const Rid &rid, const RmRecord &record,
                      Transaction *txn) {
    const auto &tab = sm_manager->db_.get_table(tab_name);
    for (const auto &index : tab.indexes) {
        auto ih = sm_manager->ihs_.at(sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
        auto key = MakeKey(index, record);
        ih->insert_entry(key.data(), rid, txn);
    }
}

inline void DeleteAll(SmManager *sm_manager, const std::string &tab_name, const RmRecord &record, Transaction *txn) {
    const auto &tab = sm_manager->db_.get_table(tab_name);
    for (const auto &index : tab.indexes) {
        auto ih = sm_manager->ihs_.at(sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
        auto key = MakeKey(index, record);
        if (!ih->delete_entry(key.data(), txn)) {
            throw IndexEntryNotFoundError();
        }
    }
}

inline void DeleteEntryBestEffort(IxIndexHandle *ih, const char *key, Transaction *txn) {
    try {
        ih->delete_entry(key, txn);
    } catch (...) {
    }
}

inline void InsertEntryIfMissing(IxIndexHandle *ih, const char *key, const Rid &rid, Transaction *txn) {
    try {
        Rid found;
        if (ih->get_value(key, &found, txn)) {
            return;
        }
    } catch (...) {
    }

    try {
        ih->insert_entry(key, rid, txn);
    } catch (...) {
    }
}

inline void RecoveryInsertEntry(IxIndexHandle *ih, const char *key, const Rid &rid, lsn_t page_lsn) {
    try {
        ih->insert_entry(key, rid, nullptr, page_lsn, false);
    } catch (const DuplicateKeyError &) {
        Rid found;
        if (ih->get_value(key, &found, nullptr) && found == rid) {
            return;
        }
        ih->delete_entry(key, nullptr, page_lsn, false);
        ih->insert_entry(key, rid, nullptr, page_lsn, false);
    }
}

inline void RecoveryDeleteEntry(IxIndexHandle *ih, const char *key, lsn_t page_lsn) {
    // With checkpoint recovery, indexes are not reset before redo/undo.  A dirty
    // index page evicted by the BPM before the crash may already reflect the
    // deletion on disk/OS-cache, so the entry is legitimately absent.  Treat
    // "not found" as idempotent success, mirroring RecoveryInsertEntry's handling
    // of DuplicateKeyError.
    try {
        ih->delete_entry(key, nullptr, page_lsn, false);
    } catch (const IndexEntryNotFoundError &) {
        // Entry already absent — deletion was already applied before crash.
    }
}

inline void DeleteAllBestEffort(SmManager *sm_manager, const std::string &tab_name, const RmRecord &record,
                                Transaction *txn) {
    try {
        const auto &tab = sm_manager->db_.get_table(tab_name);
        for (const auto &index : tab.indexes) {
            auto ih = sm_manager->ihs_.at(sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
            auto key = MakeKey(index, record);
            DeleteEntryBestEffort(ih, key.data(), txn);
        }
    } catch (...) {
    }
}

// abort 允许“本语句尚未把某个索引项写入”这一幂等缺失，但页访问、锁和结构异常必须上抛，
// 由事务管理器 fail-stop；不能把真实索引损坏伪装成回滚成功。
inline void DeleteAllIfPresent(SmManager *sm_manager, const std::string &tab_name, const RmRecord &record,
                               Transaction *txn) {
    const auto &tab = sm_manager->db_.get_table(tab_name);
    for (const auto &index : tab.indexes) {
        auto ih = sm_manager->ihs_.at(sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
        auto key = MakeKey(index, record);
        (void)ih->delete_entry(key.data(), txn);
    }
}

// UPDATE 回滚只撤销键值实际发生变化的索引项。未变化索引在正向 UPDATE 中从未删除，
// 若回滚时先删后补，会制造一个没有 stale 候选保护的并发漏读窗口。
inline void DeleteChangedIfPresent(SmManager *sm_manager, const std::string &tab_name,
                                   const RmRecord &old_record, const RmRecord &new_record,
                                   Transaction *txn) {
    const auto &tab = sm_manager->db_.get_table(tab_name);
    for (const auto &index : tab.indexes) {
        auto old_key = MakeKey(index, old_record);
        auto new_key = MakeKey(index, new_record);
        if (old_key == new_key) {
            continue;
        }
        auto ih = sm_manager->ihs_.at(sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
        (void)ih->delete_entry(new_key.data(), txn);
    }
}

inline void InsertAllIfMissingStrict(SmManager *sm_manager, const std::string &tab_name, const Rid &rid,
                                     const RmRecord &record, Transaction *txn) {
    const auto &tab = sm_manager->db_.get_table(tab_name);
    for (const auto &index : tab.indexes) {
        auto ih = sm_manager->ihs_.at(sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
        auto key = MakeKey(index, record);
        Rid found;
        if (ih->get_value(key.data(), &found, txn)) {
            if (found == rid) {
                continue;
            }
            throw DuplicateKeyError();
        }
        try {
            ih->insert_entry(key.data(), rid, txn);
        } catch (const DuplicateKeyError &) {
            // 并发/部分回滚重入时只接受“同一 key 已指向同一 RID”这一幂等结果。
            if (ih->get_value(key.data(), &found, txn) && found == rid) {
                continue;
            }
            throw;
        }
    }
}

inline void InsertChangedIfMissingStrict(SmManager *sm_manager, const std::string &tab_name, const Rid &rid,
                                         const RmRecord &old_record, const RmRecord &new_record,
                                         Transaction *txn) {
    const auto &tab = sm_manager->db_.get_table(tab_name);
    for (const auto &index : tab.indexes) {
        auto old_key = MakeKey(index, old_record);
        auto new_key = MakeKey(index, new_record);
        if (old_key == new_key) {
            continue;
        }
        auto ih = sm_manager->ihs_.at(sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
        Rid found;
        if (ih->get_value(old_key.data(), &found, txn)) {
            if (found == rid) {
                continue;
            }
            throw DuplicateKeyError();
        }
        try {
            ih->insert_entry(old_key.data(), rid, txn);
        } catch (const DuplicateKeyError &) {
            if (ih->get_value(old_key.data(), &found, txn) && found == rid) {
                continue;
            }
            throw;
        }
    }
}

inline void InsertAllIfMissing(SmManager *sm_manager, const std::string &tab_name, const Rid &rid,
                               const RmRecord &record, Transaction *txn) {
    try {
        const auto &tab = sm_manager->db_.get_table(tab_name);
        for (const auto &index : tab.indexes) {
            auto ih = sm_manager->ihs_.at(sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
            auto key = MakeKey(index, record);
            InsertEntryIfMissing(ih, key.data(), rid, txn);
        }
    } catch (...) {
    }
}

inline void RecoveryInsertAll(SmManager *sm_manager, const std::string &tab_name, const Rid &rid,
                              const RmRecord &record, lsn_t page_lsn) {
    const auto &tab = sm_manager->db_.get_table(tab_name);
    for (const auto &index : tab.indexes) {
        auto ih = sm_manager->ihs_.at(sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
        auto key = MakeKey(index, record);
        RecoveryInsertEntry(ih, key.data(), rid, page_lsn);
    }
}

inline void RecoveryDeleteAll(SmManager *sm_manager, const std::string &tab_name, const RmRecord &record,
                              lsn_t page_lsn) {
    const auto &tab = sm_manager->db_.get_table(tab_name);
    for (const auto &index : tab.indexes) {
        auto ih = sm_manager->ihs_.at(sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
        auto key = MakeKey(index, record);
        RecoveryDeleteEntry(ih, key.data(), page_lsn);
    }
}

inline void RecoveryUpdateAll(SmManager *sm_manager, const std::string &tab_name, const Rid &rid,
                              const RmRecord &old_record, const RmRecord &new_record, lsn_t page_lsn) {
    const auto &tab = sm_manager->db_.get_table(tab_name);
    for (const auto &index : tab.indexes) {
        auto ih = sm_manager->ihs_.at(sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols)).get();
        auto old_key = MakeKey(index, old_record);
        auto new_key = MakeKey(index, new_record);
        if (old_key == new_key) {
            RecoveryInsertEntry(ih, old_key.data(), rid, page_lsn);
            continue;
        }
        RecoveryDeleteEntry(ih, old_key.data(), page_lsn);
        RecoveryInsertEntry(ih, new_key.data(), rid, page_lsn);
    }
}

}  // namespace IndexHelper
