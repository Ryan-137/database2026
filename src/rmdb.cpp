/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <execinfo.h>
#include <netinet/in.h>
#include <readline/history.h>
#include <readline/readline.h>
#include <signal.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <strings.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <unordered_set>
#include <vector>

#include "errors.h"
#include "optimizer/optimizer.h"
#include "recovery/log_recovery.h"
#include "optimizer/plan.h"
#include "optimizer/planner.h"
#include "parser/parser_api.h"
#include "portal.h"
#include "analyze/analyze.h"
#include "common/output_file.h"
#include "transaction/conflict_waiter.h"

#define SOCK_PORT 8765
#define MAX_CONN_LIMIT 8

static volatile sig_atomic_t should_exit = 0;
static volatile sig_atomic_t server_listen_fd = -1;

namespace {

std::mutex client_handlers_mutex;
std::condition_variable client_handlers_cv;
std::unordered_set<int> active_client_fds;
std::mutex client_threads_mutex;
std::vector<pthread_t> client_threads;

class ClientHandlerGuard {
   public:
    explicit ClientHandlerGuard(int fd) : fd_(fd) {
        std::lock_guard<std::mutex> lock(client_handlers_mutex);
        active_client_fds.insert(fd_);
    }

    ~ClientHandlerGuard() {
        {
            std::lock_guard<std::mutex> lock(client_handlers_mutex);
            active_client_fds.erase(fd_);
        }
        client_handlers_cv.notify_all();
    }

    ClientHandlerGuard(const ClientHandlerGuard &) = delete;
    ClientHandlerGuard &operator=(const ClientHandlerGuard &) = delete;

   private:
    int fd_;
};

void ShutdownActiveClientSockets() {
    std::vector<int> fds;
    {
        std::lock_guard<std::mutex> lock(client_handlers_mutex);
        fds.assign(active_client_fds.begin(), active_client_fds.end());
    }
    for (int fd : fds) {
        shutdown(fd, SHUT_RDWR);
    }
}

void WaitForClientHandlersToDrain() {
    std::unique_lock<std::mutex> lock(client_handlers_mutex);
    client_handlers_cv.wait(lock, [] { return active_client_fds.empty(); });
}

void RememberClientThread(pthread_t thread_id) {
    std::lock_guard<std::mutex> lock(client_threads_mutex);
    client_threads.push_back(thread_id);
}

void JoinClientThreads() {
    std::vector<pthread_t> threads;
    {
        std::lock_guard<std::mutex> lock(client_threads_mutex);
        threads.swap(client_threads);
    }
    for (pthread_t thread_id : threads) {
        pthread_join(thread_id, nullptr);
    }
}

}  // namespace

static size_t InitialBufferPoolPages() {
    const char *value = std::getenv("RMDB_BPM_PAGES");
    if (value == nullptr || value[0] == '\0' || value[0] == '-') return BUFFER_POOL_SIZE;
    char *end = nullptr;
    errno = 0;
    unsigned long long parsed = std::strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0 ||
        parsed > static_cast<unsigned long long>(std::numeric_limits<frame_id_t>::max())) {
        return BUFFER_POOL_SIZE;
    }
    return static_cast<size_t>(parsed);
}

// 构建全局所需的管理器对象
auto disk_manager = std::make_unique<DiskManager>();
auto buffer_pool_manager = std::make_unique<BufferPoolManager>(InitialBufferPoolPages(), disk_manager.get());
auto rm_manager = std::make_unique<RmManager>(disk_manager.get(), buffer_pool_manager.get());
auto ix_manager = std::make_unique<IxManager>(disk_manager.get(), buffer_pool_manager.get());
auto sm_manager = std::make_unique<SmManager>(disk_manager.get(), buffer_pool_manager.get(), rm_manager.get(), ix_manager.get());
auto lock_manager = std::make_unique<LockManager>();
auto txn_manager = std::make_unique<TransactionManager>(lock_manager.get(), sm_manager.get());
auto planner = std::make_unique<Planner>(sm_manager.get());
auto optimizer = std::make_unique<Optimizer>(sm_manager.get(), planner.get());
auto ql_manager = std::make_unique<QlManager>(sm_manager.get(), txn_manager.get(), nullptr);
auto log_manager = std::make_unique<LogManager>(disk_manager.get());
auto recovery = std::make_unique<RecoveryManager>(disk_manager.get(), buffer_pool_manager.get(), sm_manager.get(),
                                                  txn_manager.get());
auto portal = std::make_unique<Portal>(sm_manager.get(), txn_manager.get());
auto analyze = std::make_unique<Analyze>(sm_manager.get());

namespace {

enum class ParallelStage { kOff, kReadRead, kReadWrite };

ParallelStage ParseParallelStage() {
    const char *stage = std::getenv("RMDB_PARALLEL_EXEC_STAGE");
    if (stage != nullptr) {
        if (strcmp(stage, "0") == 0 || strcasecmp(stage, "off") == 0) {
            return ParallelStage::kOff;
        }
        if (strcasecmp(stage, "A") == 0 || strcasecmp(stage, "readread") == 0 ||
            strcasecmp(stage, "read_read") == 0) {
            return ParallelStage::kReadRead;
        }
        if (strcasecmp(stage, "B") == 0 || strcasecmp(stage, "readwrite") == 0 ||
            strcasecmp(stage, "read_write") == 0) {
            return ParallelStage::kReadWrite;
        }
    }
    return ParallelStage::kReadWrite;
}

ParallelStage CurrentStage() {
    static const ParallelStage stage = ParseParallelStage();
    return stage;
}

constexpr bool IsPowerOfTwo(size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

constexpr size_t kRequestedStatementGateShards = STATEMENT_GATE_SHARDS;
static_assert(IsPowerOfTwo(kRequestedStatementGateShards), "STATEMENT_GATE_SHARDS must be a power of two");

constexpr size_t kStatementGateShardCount = kRequestedStatementGateShards;

class ShardedGate {
   public:
    explicit ShardedGate(size_t shard_count) : shards_(shard_count), mask_(shard_count - 1) {
        if (!IsPowerOfTwo(shard_count)) {
            std::abort();
        }
    }

    size_t slot_for_current_thread() const {
        static thread_local size_t slot =
            next_thread_slot_.fetch_add(1, std::memory_order_relaxed);
        return slot & mask_;
    }

    void lock_shared(size_t slot) { shards_[slot & mask_].mu.lock_shared(); }

    void unlock_shared(size_t slot) { shards_[slot & mask_].mu.unlock_shared(); }

    // Exclusive statements lock every shard in one global order and release in
    // reverse order, preserving the statement gate as the outermost lock layer.
    void lock_exclusive() {
        size_t locked = 0;
        try {
            for (; locked < shards_.size(); ++locked) {
                shards_[locked].mu.lock();
            }
        } catch (...) {
            while (locked > 0) {
                shards_[--locked].mu.unlock();
            }
            throw;
        }
    }

    void unlock_exclusive() {
        for (size_t i = shards_.size(); i > 0; --i) {
            shards_[i - 1].mu.unlock();
        }
    }

   private:
    struct alignas(64) Shard {
        std::shared_mutex mu;
    };

    std::vector<Shard> shards_;
    size_t mask_;
    inline static std::atomic<size_t> next_thread_slot_{0};
};

class StatementGateLock {
   public:
    explicit StatementGateLock(ShardedGate &gate, bool exclusive = true) : gate_(gate), exclusive_(exclusive) {
        lock_shared_or_exclusive(exclusive_);
    }

    ~StatementGateLock() {
        if (owns_) {
            unlock();
        }
    }

    StatementGateLock(const StatementGateLock &) = delete;
    StatementGateLock &operator=(const StatementGateLock &) = delete;

    void lock_shared_or_exclusive(bool exclusive) {
        exclusive_ = exclusive;
        if (exclusive_) {
            gate_.lock_exclusive();
        } else {
            slot_ = gate_.slot_for_current_thread();
            gate_.lock_shared(slot_);
        }
        owns_ = true;
    }

    void lock() { lock_shared_or_exclusive(exclusive_); }

    void lock_exclusive() { lock_shared_or_exclusive(true); }

    void unlock() {
        if (!owns_) {
            return;
        }
        if (exclusive_) {
            gate_.unlock_exclusive();
        } else {
            gate_.unlock_shared(slot_);
        }
        owns_ = false;
    }

    bool is_exclusive() const { return owns_ && exclusive_; }

   private:
    ShardedGate &gate_;
    size_t slot_{0};
    bool exclusive_;
    bool owns_{false};
};

bool StageAllowsSharedGate() {
    return CurrentStage() != ParallelStage::kOff;
}

bool InitialStatementGateExclusive() {
    return !StageAllowsSharedGate();
}

bool IsReadOnlyPlanTree(const std::shared_ptr<Plan> &plan) {
    if (plan == nullptr) {
        return false;
    }
    switch (plan->tag) {
        case T_SeqScan:
        case T_IndexScan:
            return true;
        case T_Projection:
            return IsReadOnlyPlanTree(std::static_pointer_cast<ProjectionPlan>(plan)->subplan_);
        case T_Aggregate:
            return IsReadOnlyPlanTree(std::static_pointer_cast<AggregatePlan>(plan)->subplan_);
        case T_Sort:
            return IsReadOnlyPlanTree(std::static_pointer_cast<SortPlan>(plan)->subplan_);
        case T_NestLoop:
        case T_SortMerge: {
            auto join = std::static_pointer_cast<JoinPlan>(plan);
            return IsReadOnlyPlanTree(join->left_) && IsReadOnlyPlanTree(join->right_);
        }
        case T_IndexNestedLoop:
            return IsReadOnlyPlanTree(std::static_pointer_cast<IndexNestedLoopJoinPlan>(plan)->left_);
        case T_Union: {
            auto union_plan = std::static_pointer_cast<UnionPlan>(plan);
            for (const auto &child : union_plan->subplans_) {
                if (!IsReadOnlyPlanTree(child)) {
                    return false;
                }
            }
            return true;
        }
        default:
            return false;
    }
}

bool IsReadOnlyStatementPlan(const std::shared_ptr<Plan> &plan) {
    if (plan == nullptr) {
        return false;
    }
    if (plan->tag == T_ExplainAnalyze) {
        return true;
    }
    if (plan->tag == T_select) {
        auto dml = std::static_pointer_cast<DMLPlan>(plan);
        return IsReadOnlyPlanTree(dml->subplan_);
    }
    return IsReadOnlyPlanTree(plan);
}

bool IsExclusiveOnlyStatementPlan(const std::shared_ptr<Plan> &plan) {
    if (plan == nullptr) {
        return true;
    }
    switch (plan->tag) {
        case T_CreateTable:
        case T_CreateStaticCheckpoint:
        case T_DropTable:
        case T_CreateIndex:
        case T_DropIndex:
        case T_SetKnob:
            return true;
        default:
            return false;
    }
}

bool ShouldTakeExclusiveStatementGate(const std::shared_ptr<Plan> &plan) {
    switch (CurrentStage()) {
        case ParallelStage::kOff:
            return true;
        case ParallelStage::kReadRead:
            return !IsReadOnlyStatementPlan(plan);
        case ParallelStage::kReadWrite:
            return IsExclusiveOnlyStatementPlan(plan);
    }
    return true;
}

// Stage 0 keeps the old serialization behavior. Stage A admits read/read
// sharing, and Stage B admits DML/transaction control sharing while keeping
// DDL, checkpoint, load, and global knob changes behind the exclusive gate.
ShardedGate statement_gate(kStatementGateShardCount);

}  // namespace
static void InitServerStdout() {
    if (std::getenv("RMDB_VERBOSE") != nullptr) {
        return;
    }
    std::cout.setstate(std::ios_base::badbit);
    int fd = open("/dev/null", O_WRONLY);
    if (fd < 0) {
        return;
    }
    dup2(fd, STDOUT_FILENO);
    close(fd);
}

// 崩溃诊断：normal phase 若因写入越界/空指针等收到致命信号，默认只表现为
// “server stops running”，无任何线索。这里在收到致命信号时把符号化调用栈写到
// stderr（评测采集的 server log 未被重定向）以及 rmdb_crash.log，把不透明的
// “停止运行”变成可定位的栈信息。仅使用 async-signal-safe 的 write/backtrace_*。
static void WriteAll(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t r = write(fd, buf + off, len - off);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            break;
        }
        off += static_cast<size_t>(r);
    }
}

static void DumpBacktrace(int fd, const char *tag, size_t taglen) {
    static const char kHdr[] = "\n=== RMDB FATAL SIGNAL: ";
    static const char kEnd[] = " ===\n";
    WriteAll(fd, kHdr, sizeof(kHdr) - 1);
    WriteAll(fd, tag, taglen);
    WriteAll(fd, kEnd, sizeof(kEnd) - 1);
    void *frames[64];
    int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, fd);
}

static void FatalSignalHandler(int signo) {
    const char *tag = "UNKNOWN";
    switch (signo) {
        case SIGSEGV: tag = "SIGSEGV"; break;
        case SIGABRT: tag = "SIGABRT"; break;
        case SIGBUS: tag = "SIGBUS"; break;
        case SIGFPE: tag = "SIGFPE"; break;
        default: break;
    }
    size_t taglen = strlen(tag);
    DumpBacktrace(STDERR_FILENO, tag, taglen);
    int fd = open("rmdb_crash.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        DumpBacktrace(fd, tag, taglen);
        close(fd);
    }
    signal(signo, SIG_DFL);
    raise(signo);
}

static void InstallCrashHandlers() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = FatalSignalHandler;
    sa.sa_flags = SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
}


static bool WriteClientResponse(int fd, const std::string &response) {
    size_t written = 0;
    const size_t len = response.size() + 1;  // 保持既有以 NUL 结尾的客户端协议。
    const char *data = response.c_str();
    while (written < len) {
        ssize_t ret = send(fd, data + written, len - written, MSG_NOSIGNAL);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (ret == 0) {
            return false;
        }
        written += static_cast<size_t>(ret);
    }
    return true;
}

void sigint_handler(int signo) {
    (void)signo;
    should_exit = 1;
    int fd = static_cast<int>(server_listen_fd);
    if (fd >= 0) {
        server_listen_fd = -1;
        close(fd);
    }
}

static void SetResponse(std::string *response, const std::string &msg) {
    response->assign(msg);
}

static void AppendOutput(const std::string &msg, bool enabled) {
    append_output_file(msg, enabled);
}

namespace {

enum class UtilityParseResult { NOT_MATCHED, VALID, INVALID };

std::string AsciiLower(std::string value) {
    for (char &ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

void StripOptionalSemicolon(std::string *token) {
    if (!token->empty() && token->back() == ';') token->pop_back();
}

std::string NormalizeControlCommand(const char *sql) {
    std::string text(sql == nullptr ? "" : sql);
    auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    auto begin = std::find_if(text.begin(), text.end(), not_space);
    auto end = std::find_if(text.rbegin(), text.rend(), not_space).base();
    text = begin < end ? std::string(begin, end) : "";
    if (!text.empty() && text.back() == ';') {
        text.pop_back();
        begin = std::find_if(text.begin(), text.end(), not_space);
        end = std::find_if(text.rbegin(), text.rend(), not_space).base();
        text = begin < end ? std::string(begin, end) : "";
    }
    return AsciiLower(std::move(text));
}

UtilityParseResult ParseLoadCommand(const std::string &sql, std::string *file_name, std::string *table_name) {
    std::istringstream input(sql);
    std::string load;
    if (!(input >> load) || AsciiLower(load) != "load") return UtilityParseResult::NOT_MATCHED;

    std::string into;
    std::string extra;
    if (!(input >> *file_name >> into >> *table_name) || AsciiLower(into) != "into") {
        return UtilityParseResult::INVALID;
    }
    StripOptionalSemicolon(table_name);
    if (table_name->empty() || (input >> extra)) return UtilityParseResult::INVALID;
    return UtilityParseResult::VALID;
}

UtilityParseResult ParseOutputFileCommand(const std::string &sql, bool *enabled) {
    std::istringstream input(sql);
    std::string set;
    if (!(input >> set) || AsciiLower(set) != "set") return UtilityParseResult::NOT_MATCHED;

    std::string option;
    std::string value;
    std::string extra;
    if (!(input >> option >> value) || AsciiLower(option) != "output_file") {
        return UtilityParseResult::NOT_MATCHED;
    }
    StripOptionalSemicolon(&value);
    value = AsciiLower(value);
    if ((input >> extra) || (value != "on" && value != "off")) return UtilityParseResult::INVALID;
    *enabled = value == "on";
    return UtilityParseResult::VALID;
}

}  // namespace

// 微批只临时 pin 少量页；每批只验证/等待一次 WAL，再按 fd/page_no 写回。
static constexpr size_t kDirtyCleanerBatchPages = 32;
static constexpr size_t kLegacyDirtyCleanerHighWatermarkPages = 2048;
static constexpr int kConflictRetryMax = 16;
static constexpr auto kConflictRetryBudget = std::chrono::milliseconds(120);
static constexpr auto kConflictRetryWait = std::chrono::milliseconds(20);

static size_t ResolveDirtyCleanerHighWatermark() {
    const size_t pool_pages = buffer_pool_manager->pool_size();
    if (pool_pages <= 1) return 0;
    const char *proactive = std::getenv("RMDB_ENABLE_PROACTIVE_PAGE_CLEANER");
    if (proactive != nullptr && std::strcmp(proactive, "0") != 0) {
        return std::min(kLegacyDirtyCleanerHighWatermarkPages, pool_pages - 1);
    }
    // 全局 75% 只作为失控保护，尽量让写热点驻留；默认自适应策略会在
    // 单分区接近耗尽或真实 clean-victim reserve 不足时更早介入。
    return std::max<size_t>(1, (pool_pages * 3) / 4);
}

static bool AdaptivePageCleanerEnabled() {
    const char *value = std::getenv("RMDB_ENABLE_ADAPTIVE_PAGE_CLEANER");
    return value == nullptr || std::strcmp(value, "0") != 0;
}

static bool WritebackStatsEnabled() {
    const char *value = std::getenv("RMDB_WRITEBACK_STATS");
    return value != nullptr && std::strcmp(value, "0") != 0;
}

// 判断当前正在执行的是显式事务还是单条SQL语句的事务，并更新事务ID
void SetTransaction(SessionTxnState *session, Context *context) {
    context->txn_ = session->current_txn;
    if (session->explicit_txn_active && context->txn_ != nullptr &&
        context->txn_->get_state() == TransactionState::GROWING) {
        assert(context->txn_->get_thread_id() == std::this_thread::get_id());
        context->txn_->set_txn_mode(true);
        return;
    }

    if(context->txn_ == nullptr || context->txn_->get_state() == TransactionState::COMMITTED ||
        context->txn_->get_state() == TransactionState::ABORTED || !session->explicit_txn_active) {
        context->txn_ = txn_manager->begin(nullptr, context->log_mgr_, session->next_isolation);
        session->BindTransaction(context->txn_);
        context->txn_->set_txn_mode(false);
        session->explicit_txn_active = false;
    }
}

static void CleanupSessionTransaction(SessionTxnState *session) {
    Transaction *txn = session->current_txn;
    if (txn != nullptr && txn->get_state() == TransactionState::GROWING) {
        try {
            txn_manager->abort(txn, log_manager.get());
        } catch (...) {
            session->explicit_txn_active = false;
            session->ClearTransaction();
            throw;
        }
    }
    session->explicit_txn_active = false;
    session->explicit_txn_failed = false;
    session->ClearTransaction();
}

// A statement error inside an explicit transaction aborts the whole
// transaction. Otherwise earlier writes in the same transaction could survive a
// later failure and be committed as a partial, inconsistent result.
static bool AbortFailedStatement(SessionTxnState *session, Context *context) {
    const bool was_explicit = session->explicit_txn_active;
    Transaction *txn = context == nullptr ? nullptr : context->txn_;
    if (txn == nullptr) {
        txn = session->current_txn;
    }
    if (txn != nullptr && txn->get_state() == TransactionState::GROWING) {
        try {
            txn_manager->abort(txn, log_manager.get());
        } catch (...) {
        }
    }
    session->statement_aborted = true;
    session->explicit_txn_active = false;
    session->explicit_txn_failed = was_explicit;
    session->ClearTransaction();
    if (context != nullptr) {
        context->txn_ = nullptr;
    }
    return was_explicit;
}

static bool ClientDisconnected(int fd) {
    char peer_probe;
    ssize_t peer_status = recv(fd, &peer_probe, sizeof(peer_probe), MSG_PEEK | MSG_DONTWAIT);
    return peer_status == 0 ||
           (peer_status < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
}

void *client_handler(void *sock_fd) {
    int fd = *static_cast<int *>(sock_fd);
    delete static_cast<int *>(sock_fd);
    ClientHandlerGuard client_guard(fd);

    int i_recvBytes;
    // 接收客户端发送的请求
    char data_recv[BUFFER_LENGTH + 1];
    // 需要返回给客户端的结果
    std::string response;
    SessionTxnState session;

    try {
    while (true) {
        memset(data_recv, 0, sizeof(data_recv));

        i_recvBytes = read(fd, data_recv, BUFFER_LENGTH);

        if (i_recvBytes == 0) {
            break;
        }
        if (i_recvBytes == -1) {
            std::cout << "Client read error!" << std::endl;
            break;
        }

        if (strcmp(data_recv, "exit") == 0) {
            break;
        }
        if (strcmp(data_recv, "crash") == 0) {
            std::cout << "Server crash" << std::endl;
            exit(1);
        }

        StatementGateLock statement_lock(statement_gate, InitialStatementGateExclusive());
        // A synchronous client has at most one outstanding statement. If it
        // disconnected while this request waited behind another statement,
        // do not execute a late COMMIT that the benchmark can no longer count.
        if (ClientDisconnected(fd)) {
            break;
        }
        response.clear();
        session.statement_aborted = false;
        session.statement_executed = false;

        // 显式事务一旦因冲突或执行错误回滚，尾部普通 SQL 不能被误当作
        // 隐式事务执行；COMMIT/ABORT/ROLLBACK 只消费 failed 边界，新的
        // BEGIN 则清理边界后正常建立显式事务。
        const std::string control_command = NormalizeControlCommand(data_recv);
        if (session.explicit_txn_failed) {
            if (control_command == "begin") {
                session.explicit_txn_failed = false;
                session.explicit_txn_active = false;
                session.ClearTransaction();
            } else if (control_command == "commit" || control_command == "abort" ||
                       control_command == "rollback") {
                // 冲突/错误语句已经返回过 abort 或 failure。事务结束命令只消费
                // failed 边界，保持题面中“仅一次 abort”的输出口径。
                session.explicit_txn_failed = false;
                session.explicit_txn_active = false;
                session.ClearTransaction();
                statement_lock.unlock();
                if (!WriteClientResponse(fd, response)) break;
                continue;
            } else {
                const std::string abort_response = "abort\n";
                SetResponse(&response, abort_response);
                AppendOutput(abort_response, session.output_file_enabled);
                session.statement_aborted = true;
                statement_lock.unlock();
                if (!WriteClientResponse(fd, response)) break;
                continue;
            }
        }

        // LOAD carries a filesystem path, so it is handled by this generic
        // utility-command parser. No benchmark table or schema is special-cased.
        std::string load_file;
        std::string load_table;
        UtilityParseResult load_result = ParseLoadCommand(data_recv, &load_file, &load_table);
        bool output_enabled = true;
        UtilityParseResult output_result = ParseOutputFileCommand(data_recv, &output_enabled);
        if (load_result != UtilityParseResult::NOT_MATCHED || output_result != UtilityParseResult::NOT_MATCHED) {
            if (!statement_lock.is_exclusive()) {
                statement_lock.unlock();
                statement_lock.lock_exclusive();
                if (ClientDisconnected(fd)) {
                    break;
                }
                load_file.clear();
                load_table.clear();
                load_result = ParseLoadCommand(data_recv, &load_file, &load_table);
                output_result = ParseOutputFileCommand(data_recv, &output_enabled);
            }
            try {
                if (load_result == UtilityParseResult::INVALID || output_result == UtilityParseResult::INVALID) {
                    throw RMDBError("Invalid utility command");
                }
                if (load_result == UtilityParseResult::VALID) {
                    sm_manager->load_data(load_file, load_table);
                } else {
                    session.output_file_enabled = output_enabled;
                }
            } catch (...) {
                const std::string failure = "failure\n";
                SetResponse(&response, failure);
                AppendOutput(failure, session.output_file_enabled);
            }
            statement_lock.unlock();
            if (!WriteClientResponse(fd, response)) break;
            continue;
        }

        // 开启事务，初始化系统所需的上下文信息（包括事务对象指针、锁管理器指针、日志管理器指针、存放结果的buffer、记录结果长度的变量）
        auto context = std::make_unique<Context>(lock_manager.get(), log_manager.get(), nullptr, &response,
                                                 session.output_file_enabled);
        try {
            SetTransaction(&session, context.get());
        } catch (...) {
            // begin() may throw (e.g. std::bad_alloc, UnixError from WAL flush).
            // Send failure and continue rather than letting the exception escape
            // client_handler which would call std::terminate via pthread.
            const std::string failure = "failure\n";
            SetResponse(&response, failure);
            AppendOutput(failure, session.output_file_enabled);
            session.ClearTransaction();
            if (!WriteClientResponse(fd, response)) {
                break;
            }
            continue;
        }

        auto handle_transaction_abort = [&]() {
            std::string str = "abort\n";
            SetResponse(&response, str);
            AbortFailedStatement(&session, context.get());
            AppendOutput(str, session.output_file_enabled);
        };
        auto handle_error_response = [&]() {
            const bool explicit_error = session.explicit_txn_active;
            const std::string error_response = "failure\n";
            SetResponse(context->response_, error_response);
            if (!session.explicit_txn_active) {
                SessionTxnResetGuard reset_guard(&session, context.get());
                try {
                    txn_manager->abort(context->txn_, log_manager.get());
                } catch (...) {}
            }
            if (explicit_error) AbortFailedStatement(&session, context.get());
            AppendOutput(error_response, session.output_file_enabled);
        };
        auto refresh_implicit_transaction_after_gate_upgrade = [&]() {
            if (session.explicit_txn_active || context->txn_ == nullptr ||
                context->txn_->get_state() != TransactionState::GROWING) {
                return;
            }
            {
                SessionTxnResetGuard reset_guard(&session, context.get());
                txn_manager->abort(context->txn_, log_manager.get());
            }
            SetTransaction(&session, context.get());
        };

        int retry_count = 0;
        auto retry_deadline = std::chrono::steady_clock::now() + kConflictRetryBudget;
        bool statement_done = false;
        bool client_disconnected = false;
        while (!statement_done) {
            try {
                std::shared_ptr<ast::TreeNode> parse_tree;
                std::shared_ptr<Plan> plan;
                auto parse_analyze_plan = [&]() {
                    parse_tree.reset();
                    plan.reset();
                    int parse_result = parser::Parse(data_recv, &parse_tree);
                    if (parse_result == 0 && parse_tree != nullptr) {
                        std::shared_ptr<Query> query = analyze->do_analyze(parse_tree);
                        plan = optimizer->plan_query(query, context.get());
                    }
                    return parse_result;
                };

                int parse_result = parse_analyze_plan();
                if (parse_result == 0) {
                    if (plan != nullptr) {
                        if (!statement_lock.is_exclusive() && ShouldTakeExclusiveStatementGate(plan)) {
                            statement_lock.unlock();
                            statement_lock.lock_exclusive();
                            if (ClientDisconnected(fd)) {
                                client_disconnected = true;
                                statement_done = true;
                                continue;
                            }
                            refresh_implicit_transaction_after_gate_upgrade();
                            // A concurrent exclusive statement may have changed schema while
                            // this session upgraded the gate. Rebuild the parse/analyze/plan
                            // product under the exclusive gate before executing it.
                            parse_result = parse_analyze_plan();
                            if (parse_result != 0) {
                                handle_error_response();
                                statement_done = true;
                                continue;
                            }
                            if (plan == nullptr) {
                                statement_done = true;
                                continue;
                            }
                        }
                        if (session.explicit_txn_active && std::dynamic_pointer_cast<DDLPlan>(plan) != nullptr) {
                            const std::string failure = "failure\n";
                            SetResponse(&response, failure);
                            AppendOutput(failure, session.output_file_enabled);
                            statement_done = true;
                            continue;
                        }
                        std::shared_ptr<PortalStmt> portalStmt = portal->start(plan, context.get());
                        portal->run(portalStmt, ql_manager.get(), &session, context.get());
                        portal->drop();
                        session.statement_executed = true;
                    }
                } else {
                    handle_error_response();
                }
                if (retry_count > 0 && session.statement_executed && !session.statement_aborted) {
                    GlobalConflictRetryStats().RecordSuccess();
                }
                statement_done = true;
            } catch (StatementConflictRetry &) {
                GlobalConflictRetryStats().RecordAttempt();
                if (++retry_count > kConflictRetryMax ||
                    std::chrono::steady_clock::now() >= retry_deadline) {
                    GlobalConflictRetryStats().RecordExhausted();
                    handle_transaction_abort();
                    statement_done = true;
                    continue;
                }
                std::uint64_t epoch = GlobalConflictWaiter().CurrentEpochAtomic();
                statement_lock.unlock();
                // Never wait while holding statement_gate: the conflicting owner may
                // need the gate to commit/abort and publish the epoch notification.
                GlobalConflictWaiter().WaitForRelease(epoch, kConflictRetryWait);
                statement_lock.lock();
                if (ClientDisconnected(fd)) {
                    client_disconnected = true;
                    statement_done = true;
                }
            } catch (TransactionAbortException &) {
                handle_transaction_abort();
                statement_done = true;
            } catch (RMDBError &) {
                handle_error_response();
                statement_done = true;
            } catch (std::exception &) {
                handle_error_response();
                statement_done = true;
            } catch (...) {
                handle_error_response();
                statement_done = true;
            }
        }
        if (client_disconnected) {
            break;
        }
        // future TODO: 格式化 sql_handler.result, 传给客户端
        // send result with fixed format, use protobuf in the future
        // 如果是单条语句，需要按照一个完整的事务来执行，所以执行完当前语句后，自动提交事务。
        // commit()/abort()内部有RAII确保active_txn_count_总能递减，此处吞掉异常以防
        // 自动提交失败时线程因未处理异常终止。必须先结束隐式事务再向客户端写回；
        // 否则客户端提前断开会让已经执行过的语句跳过commit/abort收尾，污染checkpoint状态。
        if(!session.explicit_txn_active && !session.statement_aborted && session.statement_executed &&
           context->txn_ != nullptr && context->txn_->get_state() == TransactionState::GROWING)
        {
            Transaction *txn = context->txn_;
            SessionTxnResetGuard reset_guard(&session, context.get());
            try {
                txn_manager->commit(txn, context->log_mgr_);
            } catch (TransactionAbortException &) {
                // SERIALIZABLE 的危险结构可能直到 auto-commit 前的冻结校验才成立。
                // 此处位于主 statement catch 之外，必须覆盖先前 SELECT/DML 响应并
                // 完整回滚；否则客户端会收到空响应或已失效的结果集。
                const std::string abort_response = "abort\n";
                SetResponse(&response, abort_response);
                session.statement_aborted = true;
                if (txn != nullptr && txn->get_state() == TransactionState::GROWING) {
                    try {
                        txn_manager->abort(txn, log_manager.get());
                    } catch (...) {}
                }
                AppendOutput(abort_response, session.output_file_enabled);
            } catch (...) {
                // commit 在 RetireTransaction 前抛出时对象仍存活，先完成 best-effort 回滚。
                if (txn != nullptr && txn->get_state() == TransactionState::GROWING) {
                    try {
                        txn_manager->abort(txn, log_manager.get());
                    } catch (...) {}
                }
            }
        }
        statement_lock.unlock();
        if (!WriteClientResponse(fd, response)) {
            break;
        }
    }
    } catch (std::exception &e) {
        try {
            AppendOutput("failure\n", session.output_file_enabled);
        } catch (...) {}
    } catch (...) {
        try {
            AppendOutput("failure\n", session.output_file_enabled);
        } catch (...) {}
    }

    // Clear
    try {
        StatementGateLock cleanup_lock(statement_gate, true);
        CleanupSessionTransaction(&session);
    } catch (...) {}
    close(fd);           // close a file descriptor.
    return nullptr;
}

void start_server() {
    pthread_attr_t thread_attr;
    pthread_attr_init(&thread_attr);
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    pthread_attr_setstacksize(&thread_attr, 8 * 1024 * 1024);
#else
    pthread_attr_setstacksize(&thread_attr, 256 * 1024);
#endif

    int sockfd_server;
    int fd_temp;
    struct sockaddr_in s_addr_in {};

    // 初始化连接
    sockfd_server = socket(AF_INET, SOCK_STREAM, 0);  // ipv4,TCP
    assert(sockfd_server != -1);
    server_listen_fd = sockfd_server;
    int val = 1;
    setsockopt(sockfd_server, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    // before bind(), set the attr of structure sockaddr.
    memset(&s_addr_in, 0, sizeof(s_addr_in));
    s_addr_in.sin_family = AF_INET;
    s_addr_in.sin_addr.s_addr = htonl(INADDR_ANY);
    s_addr_in.sin_port = htons(SOCK_PORT);
    fd_temp = bind(sockfd_server, (struct sockaddr *)(&s_addr_in), sizeof(s_addr_in));
    if (fd_temp == -1) {
        std::cout << "Bind error!" << std::endl;
        exit(1);
    }

    fd_temp = listen(sockfd_server, MAX_CONN_LIMIT);
    if (fd_temp == -1) {
        std::cout << "Listen error!" << std::endl;
        exit(1);
    }

    while (!should_exit) {
        pthread_t thread_id;
        struct sockaddr_in s_addr_client {};
        int client_length = sizeof(s_addr_client);

        // Block here. Until server accepts a new connection.
        int sockfd = accept(sockfd_server, (struct sockaddr *)(&s_addr_client), (socklen_t *)(&client_length));
        if (sockfd == -1) {
            if (should_exit) {
                break;
            }
            std::cout << "Accept error!" << std::endl;
            if (errno == EINTR || errno == EMFILE || errno == ENFILE || errno == EAGAIN || errno == ECONNABORTED) {
                usleep(1000);
            }
            continue;  // ignore current socket ,continue while loop.
        }

        // 和客户端建立连接，并开启一个线程负责处理客户端请求
        auto *client_fd = new int(sockfd);
        if (pthread_create(&thread_id, &thread_attr, &client_handler, client_fd) != 0) {
            close(sockfd);
            delete client_fd;
            std::cout << "Create thread fail!" << std::endl;
            continue;
        }
        RememberClientThread(thread_id);

    }
    pthread_attr_destroy(&thread_attr);

    // Clear
    std::cout << " Try to close all client-connection.\n";
    ShutdownActiveClientSockets();
    if (server_listen_fd >= 0) {
        close(sockfd_server);
        server_listen_fd = -1;
    }
    WaitForClientHandlersToDrain();
    JoinClientThreads();
    buffer_pool_manager->StopPageCleaner();
    log_manager->StopFlushThread();
    log_manager->flush_log_to_disk();
    sm_manager->close_db();
    if (WritebackStatsEnabled()) {
        std::cerr << buffer_pool_manager->writeback_stats() << '\n';
    }
    GlobalConflictRetryStats().PrintToStderr();
    std::cout << " DB has been closed.\n";
    std::cout << "Server shuts down." << std::endl;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        // 需要指定数据库名称
        std::cerr << "Usage: " << argv[0] << " <database>" << std::endl;
        exit(1);
    }

    signal(SIGINT, sigint_handler);
    signal(SIGPIPE, SIG_IGN);
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    InstallCrashHandlers();
#endif
    InitServerStdout();
    try {
        std::cout << "\n"
                     "  _____  __  __ _____  ____  \n"
                     " |  __ \\|  \\/  |  __ \\|  _ \\ \n"
                     " | |__) | \\  / | |  | | |_) |\n"
                     " |  _  /| |\\/| | |  | |  _ < \n"
                     " | | \\ \\| |  | | |__| | |_) |\n"
                     " |_|  \\_\\_|  |_|_____/|____/ \n"
                     "\n"
                     "Welcome to RMDB!\n"
                     "Type 'help;' for help.\n"
                     "\n";
        // Database name is passed by args
        std::string db_name = argv[1];
        if (!sm_manager->is_dir(db_name)) {
            // Database not found, create a new one
            sm_manager->create_db(db_name);
        }
        // Open database
        sm_manager->open_db(db_name);
        buffer_pool_manager->set_wal_barrier(
            [](lsn_t lsn) { return log_manager->has_record_lsn(lsn); },
            [](lsn_t lsn) { log_manager->FlushUpTo(lsn); },
            [](const std::vector<lsn_t> &lsns) { log_manager->FlushPageLsns(lsns); });
        log_manager->init_from_disk();

        // recovery database
        recovery->analyze();
        recovery->redo();
        recovery->undo();
        log_manager->init_from_disk();

        log_manager->StartFlushThread();
        buffer_pool_manager->StartPageCleaner(kDirtyCleanerBatchPages, ResolveDirtyCleanerHighWatermark(),
                                              AdaptivePageCleanerEnabled());
        
        // 开启服务端，开始接受客户端连接
        start_server();
    } catch (RMDBError &e) {
        buffer_pool_manager->StopPageCleaner();
        std::cerr << e.what() << std::endl;
        exit(1);
    } catch (std::exception &e) {
        buffer_pool_manager->StopPageCleaner();
        std::cerr << e.what() << std::endl;
        exit(1);
    } catch (...) {
        buffer_pool_manager->StopPageCleaner();
        std::cerr << "unknown server error" << std::endl;
        exit(1);
    }
    return 0;
}
