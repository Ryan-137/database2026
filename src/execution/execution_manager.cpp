/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "execution_manager.h"

#include "common/output_file.h"

#include <fcntl.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>

#include "executor_delete.h"
#include "executor_index_scan.h"
#include "executor_insert.h"
#include "executor_nestedloop_join.h"
#include "executor_projection.h"
#include "executor_seq_scan.h"
#include "executor_update.h"
#include "index/ix.h"
#include "record_printer.h"

namespace {

// checkpoint 分阶段打点：hang 不产生信号，无法走 backtrace。把当前阶段覆盖式写入
// 极小状态文件（避免向可能未被读取的 stderr 管道大量写入而自造阻塞），checkpoint
// 卡死时该文件内容即为卡住的阶段。仅在设置 RMDB_CKPT_TRACE 时启用，默认零开销。
void CheckpointPhase(const char *phase) {
    static const bool enabled = std::getenv("RMDB_CKPT_TRACE") != nullptr;
    if (!enabled || phase == nullptr) {
        return;
    }
    int fd = open("rmdb_ckpt_phase.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return;
    }
    size_t len = strlen(phase);
    ssize_t ignore = write(fd, phase, len);
    ignore = write(fd, "\n", 1);
    (void)ignore;
    close(fd);
}

class CheckpointGateGuard {
   public:
    CheckpointGateGuard(TransactionManager *txn_mgr, size_t ignored_active_txn_count)
        : txn_mgr_(txn_mgr),
          active_(txn_mgr_ != nullptr && txn_mgr_->BeginStaticCheckpoint(ignored_active_txn_count)) {}

    ~CheckpointGateGuard() {
        if (active_) {
            txn_mgr_->EndStaticCheckpoint();
        }
    }

    bool active() const { return active_; }

   private:
    TransactionManager *txn_mgr_;
    bool active_;
};

}  // namespace

const char *help_info = "Supported SQL syntax:\n"
                   "  command ;\n"
                   "command:\n"
                   "  CREATE TABLE table_name (column_name type [, column_name type ...])\n"
                   "  DROP TABLE table_name\n"
                   "  CREATE INDEX table_name (column_name)\n"
                   "  DROP INDEX table_name (column_name)\n"
                   "  INSERT INTO table_name VALUES (value [, value ...])\n"
                   "  DELETE FROM table_name [WHERE where_clause]\n"
                   "  UPDATE table_name SET column_name = value [, column_name = value ...] [WHERE where_clause]\n"
                   "  SELECT selector FROM table_name [WHERE where_clause]\n"
                   "type:\n"
                   "  {INT | FLOAT | CHAR(n)}\n"
                   "where_clause:\n"
                   "  condition [AND condition ...]\n"
                   "condition:\n"
                   "  column op {column | value}\n"
                   "column:\n"
                   "  [table_name.]column_name\n"
                   "op:\n"
                   "  {= | <> | < | > | <= | >=}\n"
                   "selector:\n"
                   "  {* | column [, column ...]}\n";

// 主要负责执行DDL语句
void QlManager::run_mutli_query(std::shared_ptr<Plan> plan, Context *context){
    if (auto x = std::dynamic_pointer_cast<DDLPlan>(plan)) {
        switch(x->tag) {
            case T_CreateTable:
            {
                sm_manager_->create_table(x->tab_name_, x->cols_, context);
                break;
            }
            case T_DropTable:
            {
                sm_manager_->drop_table(x->tab_name_, context);
                break;
            }
            case T_CreateIndex:
            {
                sm_manager_->create_index(x->tab_name_, x->tab_col_names_, context);
                break;
            }
            case T_DropIndex:
            {
                sm_manager_->drop_index(x->tab_name_, x->tab_col_names_, context);
                break;
            }
            default:
                throw InternalError("Unexpected field type");
                break;  
        }
    }
}

// 执行help; show tables; desc table; begin; commit; abort;语句
void QlManager::run_cmd_utility(std::shared_ptr<Plan> plan, SessionTxnState *session, Context *context) {
    if (auto x = std::dynamic_pointer_cast<OtherPlan>(plan)) {
        switch(x->tag) {
            case T_Help:
            {
                context->ResetResponse(help_info);
                break;
            }
            case T_ShowTable:
            {
                sm_manager_->show_tables(context);
                break;
            }
            case T_ShowIndex:
            {
                sm_manager_->show_index(x->tab_name_,context);
                break;
            }
            case T_DescTable:
            {
                sm_manager_->desc_table(x->tab_name_, context);
                break;
            }
            case T_Transaction_begin:
            {
                // Repeated BEGIN inside an explicit transaction is a no-op for Q8.
                if (!session->explicit_txn_active) {
                    context->txn_->set_txn_mode(true);
                    session->explicit_txn_active = true;
                    session->BindTransaction(context->txn_);
                }
                break;
            }
            case T_Transaction_commit:
            {
                Transaction *txn = session->current_txn;
                context->txn_ = txn;
                SessionTxnResetGuard reset_guard(session, context);
                try {
                    txn_mgr_->commit(txn, context->log_mgr_);
                } catch (...) {
                    // commit 在持久化完成前失败时仍需回滚；RetireTransaction 之后没有抛出点。
                    if (txn != nullptr && txn->get_state() == TransactionState::GROWING) {
                        try {
                            txn_mgr_->abort(txn, context->log_mgr_);
                        } catch (...) {
                        }
                    }
                    throw;
                }
                session->explicit_txn_active = false;
                sm_manager_->get_bpm()->NudgePageCleaner();
                break;
            }
            case T_Transaction_rollback:
            {
                context->txn_ = session->current_txn;
                SessionTxnResetGuard reset_guard(session, context);
                txn_mgr_->abort(context->txn_, context->log_mgr_);
                session->explicit_txn_active = false;
                session->statement_aborted = true;
                sm_manager_->get_bpm()->NudgePageCleaner();
                break;
            }
            case T_Transaction_abort:
            {
                context->txn_ = session->current_txn;
                SessionTxnResetGuard reset_guard(session, context);
                txn_mgr_->abort(context->txn_, context->log_mgr_);
                session->explicit_txn_active = false;
                session->statement_aborted = true;
                sm_manager_->get_bpm()->NudgePageCleaner();
                break;
            }
            case T_CreateStaticCheckpoint:
            {
                // 对齐 rmdb-main 的静态检查点触发语义（见 rmdb-main
                // execution_manager.cpp::T_StaticCheckpoint 与
                // sm_manager.cpp::create_static_checkpoint）：无论当前是否存在活跃
                // 事务，都执行“刷日志 -> 追加 CHECKPOINT 记录 -> 刷元数据/表/索引
                // 全部页”。save_restart 仅在静默态（除本 checkpoint 语句自身的隐式
                // 事务外，无其它处于 GROWING 的活跃事务）为真：为真才写指向本
                // checkpoint 的有效 restart 偏移，否则写 0 使下次恢复从日志头全量
                // 重放。不再在有活跃事务时整体 no-op（旧实现下 mid-transaction 的
                // checkpoint 会被完全跳过，导致评测的 with_checkpoint 场景始终没有
                // 可用检查点、恢复退化为全量重放）。
                txn_id_t ignore_txn_id = (context != nullptr && context->txn_ != nullptr &&
                                          context->txn_->get_state() == TransactionState::GROWING &&
                                          !context->txn_->get_txn_mode())
                                             ? context->txn_->get_transaction_id()
                                             : INVALID_TXN_ID;
                size_t ignored_active_txn_count = ignore_txn_id == INVALID_TXN_ID ? 0 : 1;
                CheckpointGateGuard checkpoint_guard(txn_mgr_, ignored_active_txn_count);
                bool save_restart = checkpoint_guard.active();

                if (save_restart) {
                    // 仅静默态物理清除已提交逻辑删除记录：此时不存在仍需读取旧版本的
                    // 活跃事务，落盘的堆只保留存活记录，恢复重建索引时不会让已删除
                    // 记录复活。必须在刷页之前执行。
                    CheckpointPhase("ckpt:vacuum");
                    txn_mgr_->VacuumCommittedDeletes();
                }

                CheckpointPhase("ckpt:flush_log");
                context->log_mgr_->flush_log_to_disk();

                lsn_t checkpoint_lsn = context->log_mgr_->AppendCheckpointRecord(
                    txn_mgr_->GetNextTxnId(), txn_mgr_->GetNextTimestamp(), txn_mgr_->GetLastCommitTs(),
                    INVALID_LSN);
                context->log_mgr_->flush_log_to_disk();

                CheckpointPhase("ckpt:flush_meta");
                sm_manager_->flush_meta();
                CheckpointPhase("ckpt:flush_tables");
                for (auto &entry : sm_manager_->fhs_) {
                    entry.second->flush_file_hdr();
                    sm_manager_->get_bpm()->flush_all_pages(entry.second->GetFd());
                }
                CheckpointPhase("ckpt:flush_indexes");
                for (auto &entry : sm_manager_->ihs_) {
                    entry.second->flush_file_hdr();
                    sm_manager_->get_bpm()->flush_all_pages(entry.second->GetFd());
                }

                if (save_restart) {
                    // 原子写入（tmp + rename）指向本 checkpoint 的有效恢复起点。
                    CheckpointPhase("ckpt:restart_file");
                    context->log_mgr_->WriteRestartFile(checkpoint_lsn);
                    context->log_mgr_->MarkDurablePrefix(checkpoint_lsn);
                } else {
                    // 非静默态：写 0，恢复时回退为从日志头全量重放（与 rmdb-main
                    // 的 ofs << (save_restart ? checkpoint_offset : 0) 行为一致）。
                    context->log_mgr_->WriteRestartFile(0);
                }
                CheckpointPhase("ckpt:done");
                // M9 test hook (env-gated, stderr-only, default off): in the
                // quiescent path every partition's dirty set must be empty after
                // flushing all table/index files. Used by the Q10 recovery gate
                // to assert cross-partition "no missed flush". Never affects
                // client output or output.txt.
                if (save_restart) {
                    if (const char *dbg = std::getenv("RMDB_BPM_CKPT_DIRTY_LOG");
                        dbg != nullptr && dbg[0] == '1') {
                        size_t dc = sm_manager_->get_bpm()->dirty_page_count();
                        std::cerr << "[CKPT_DIRTY] dirty_page_count=" << dc << std::endl;
                    }
                }
                break;
            }
            default:
                throw InternalError("Unexpected field type");
                break;
        }

    } else if(auto x = std::dynamic_pointer_cast<SetKnobPlan>(plan)) {
        switch (x->set_knob_type_)
        {
        case ast::SetKnobType::EnableNestLoop: {
            planner_->set_enable_nestedloop_join(x->bool_value_);
            break;
        }
        case ast::SetKnobType::EnableSortMerge: {
            planner_->set_enable_sortmerge_join(x->bool_value_);
            break;
        }
        default: {
            throw RMDBError("Not implemented!\n");
            break;
        }
        }
    } else if (auto x = std::dynamic_pointer_cast<ExplainPlan>(plan)) {
        for (const auto &line : x->lines_) {
            std::string output = line + "\n";
            context->AppendResponse(output);
            append_output_file(output, context->output_file_enabled_);
        }
    } else if (auto x = std::dynamic_pointer_cast<SetTransactionIsolationPlan>(plan)) {
        if (session->explicit_txn_active && context->txn_ != nullptr &&
            context->txn_->get_state() == TransactionState::GROWING) {
            const std::string failure = "failure\n";
            context->ResetResponse(failure);
            append_output_file(failure, context->output_file_enabled_);
            return;
        }
        session->next_isolation = x->isolation_level_;
    }
}

// 执行select语句，select语句的输出除了需要返回客户端外，还需要写入output.txt文件中
void QlManager::select_from(std::unique_ptr<AbstractExecutor> executorTreeRoot, std::vector<TabCol> sel_cols,
                            Context *context) {
    std::vector<std::string> captions;
    captions.reserve(sel_cols.size());
    for (auto &sel_col : sel_cols) {
        captions.push_back(sel_col.col_name);
    }

    std::vector<std::vector<std::string>> rows;
    // 执行query_plan
    for (executorTreeRoot->beginTuple(); !executorTreeRoot->is_end(); executorTreeRoot->nextTuple()) {
        std::unique_ptr<RmRecord> owned_tuple;
        const RmRecord *tuple = executorTreeRoot->CurrentOrNext(&owned_tuple);
        if (tuple == nullptr) {
            continue;
        }
        std::vector<std::string> columns;
        for (auto &col : executorTreeRoot->cols()) {
            std::string col_str;
            const char *rec_buf = tuple->data + col.offset;
            if (col.type == TYPE_INT) {
                col_str = std::to_string(*reinterpret_cast<const int *>(rec_buf));
            } else if (col.type == TYPE_FLOAT) {
                col_str = std::to_string(*reinterpret_cast<const float *>(rec_buf));
            } else if (col.type == TYPE_STRING) {
                col_str = std::string(rec_buf, col.len);
                col_str.resize(strlen(col_str.c_str()));
            }
            columns.push_back(col_str);
        }
        rows.push_back(std::move(columns));
    }

    // Print header into buffer only after scan succeeds. SER SELECT may abort while scanning.
    RecordPrinter rec_printer(sel_cols.size());
    rec_printer.print_separator(context);
    rec_printer.print_record(captions, context);
    rec_printer.print_separator(context);

    std::ostringstream file_output;
    const bool write_output = context->output_file_enabled_;
    if (write_output) {
        file_output << "|";
        for(size_t i = 0; i < captions.size(); ++i) {
            file_output << " " << captions[i] << " |";
        }
        file_output << "\n";
    }

    for (auto &columns : rows) {
        rec_printer.print_record(columns, context);
        if (write_output) {
            file_output << "|";
            for(size_t i = 0; i < columns.size(); ++i) {
                file_output << " " << columns[i] << " |";
            }
            file_output << "\n";
        }
    }
    if (write_output) append_output_file(file_output.str(), true);
    // Print footer into buffer
    rec_printer.print_separator(context);
    // Print record count into buffer
    RecordPrinter::print_record_count(rows.size(), context);
}

// 执行DML语句
void QlManager::run_dml(std::unique_ptr<AbstractExecutor> exec){
    exec->Next();
}
