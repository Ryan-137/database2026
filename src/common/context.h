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

#include <string>
#include <string_view>

#include "common/config.h"
#include "transaction/transaction.h"
#include "transaction/concurrency/lock_manager.h"
#include "recovery/log_manager.h"

// class TransactionManager;

struct SessionTxnState {
    txn_id_t current_txn_id = INVALID_TXN_ID;
    Transaction *current_txn = nullptr;
    bool explicit_txn_active = false;
    bool explicit_txn_failed = false;
    bool statement_aborted = false;
    bool statement_executed = false;
    bool output_file_enabled = true;
    IsolationLevel next_isolation = IsolationLevel::SNAPSHOT_ISOLATION;

    void BindTransaction(Transaction *txn) {
        current_txn = txn;
        current_txn_id = txn == nullptr ? INVALID_TXN_ID : txn->get_transaction_id();
    }

    void ClearTransaction() {
        current_txn = nullptr;
        current_txn_id = INVALID_TXN_ID;
    }
};

class Context {
public:
    Context(LockManager *lock_mgr, LogManager *log_mgr, Transaction *txn,
            std::string *response = nullptr, bool output_file_enabled = true)
        : lock_mgr_(lock_mgr), log_mgr_(log_mgr), txn_(txn),
          response_(response), output_file_enabled_(output_file_enabled) {}

    void ResetResponse(std::string_view value) {
        if (response_ != nullptr) {
            response_->assign(value.data(), value.size());
        }
        response_truncated_ = false;
    }

    void AppendResponse(std::string_view value) {
        if (response_ != nullptr) {
            response_->append(value.data(), value.size());
        }
    }

    // 题面默认以 output.txt 作为完整结果，配套客户端每条语句只接收 8 KiB。
    // 默认会话必须保留框架原有的有界表格响应；关闭 output_file 的性能会话
    // 没有文件副本，因此继续使用动态字符串返回完整结果。
    bool TryAppendTableResponse(std::string_view value, size_t reserved_tail) {
        if (response_ == nullptr) {
            return true;
        }
        if (!output_file_enabled_) {
            response_->append(value.data(), value.size());
            return true;
        }
        if (response_truncated_ || response_->size() + reserved_tail + value.size() >= BUFFER_LENGTH) {
            response_truncated_ = true;
            return false;
        }
        response_->append(value.data(), value.size());
        return true;
    }

    bool ResponseTruncated() const { return response_truncated_; }

    LockManager *lock_mgr_;
    LogManager *log_mgr_;
    Transaction *txn_;
    std::string *response_;
    bool output_file_enabled_;
    bool response_truncated_{false};
};

// commit/abort 可能在返回前回收 Transaction；本守卫只清空裸指针，不再解引用事务对象。
class SessionTxnResetGuard {
   public:
    SessionTxnResetGuard(SessionTxnState *session, Context *context) : session_(session), context_(context) {}
    ~SessionTxnResetGuard() {
        if (session_ != nullptr) {
            session_->ClearTransaction();
        }
        if (context_ != nullptr) {
            context_->txn_ = nullptr;
        }
    }

    SessionTxnResetGuard(const SessionTxnResetGuard &) = delete;
    SessionTxnResetGuard &operator=(const SessionTxnResetGuard &) = delete;

   private:
    SessionTxnState *session_;
    Context *context_;
};
