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

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <functional>
#include <set>
#include <shared_mutex>
#include <memory>
#include <utility>
#include <vector>

#include "transaction.h"
#include "watermark.h"
#include "recovery/log_manager.h"
#include "concurrency/lock_manager.h"
#include "system/sm_manager.h"
#include "common/exception.h"

/* 系统采用的并发控制算法，当前题目中要求两阶段封锁并发控制算法 */
enum class ConcurrencyMode { TWO_PHASE_LOCKING = 0, BASIC_TO, MVCC };

/// 版本链中的第一个撤销链接，将表堆元组链接到撤销日志。
struct VersionUndoLink {
    /** 版本链中的下一个版本。 */
    UndoLink prev_;
    bool in_progress_{false};

    friend auto operator==(const VersionUndoLink &a, const VersionUndoLink &b) {
        return a.prev_ == b.prev_ && a.in_progress_ == b.in_progress_;
    }

    friend auto operator!=(const VersionUndoLink &a, const VersionUndoLink &b) { return !(a == b); }

    inline static std::optional<VersionUndoLink> FromOptionalUndoLink(std::optional<UndoLink> undo_link) {
        if (undo_link.has_value()) {
            return VersionUndoLink{*undo_link};
        }
        return std::nullopt;
    }
};

class TransactionManager {
public:
    struct VersionReaderSlot {
        // One slot is owned by one thread. Only that owner mutates depth; GC
        // threads only read epoch, so depth does not need to be atomic.
        std::atomic<std::uint64_t> epoch{0};
        std::uint32_t depth{0};
    };

    class VersionReadGuard {
    public:
        explicit VersionReadGuard(TransactionManager *manager);
        ~VersionReadGuard();
        VersionReadGuard(const VersionReadGuard &) = delete;
        VersionReadGuard &operator=(const VersionReadGuard &) = delete;

    private:
        TransactionManager *manager_{nullptr};
        VersionReaderSlot *slot_{nullptr};
        bool armed_{false};
    };

    explicit TransactionManager(LockManager *lock_manager, SmManager *sm_manager,
                             ConcurrencyMode concurrency_mode = ConcurrencyMode::TWO_PHASE_LOCKING) {
        sm_manager_ = sm_manager;
        lock_manager_ = lock_manager;
        concurrency_mode_ = concurrency_mode;
    }
    
    ~TransactionManager();

    Transaction* begin(Transaction* txn, LogManager* log_manager,
                       IsolationLevel isolation_level = IsolationLevel::SNAPSHOT_ISOLATION);

    void commit(Transaction* txn, LogManager* log_manager);

    void abort(Transaction* txn, LogManager* log_manager);

    ConcurrencyMode get_concurrency_mode() { return concurrency_mode_; }

    void set_concurrency_mode(ConcurrencyMode concurrency_mode) { concurrency_mode_ = concurrency_mode; }

    LockManager* get_lock_manager() { return lock_manager_; }

    /**
     * @description: 获取事务ID为txn_id的事务对象
     * @return {Transaction*} 事务对象的指针
     * @param {txn_id_t} txn_id 事务ID
     */    
    Transaction* get_transaction(txn_id_t txn_id) {
        if(txn_id == INVALID_TXN_ID) return nullptr;

        std::shared_lock<std::shared_mutex> lock(txn_map_mutex_);
        auto it = TransactionManager::txn_map.find(txn_id);
        assert(it != TransactionManager::txn_map.end());
        auto *res = it->second;
        lock.unlock();
        assert(res != nullptr);
        assert(res->get_thread_id() == std::this_thread::get_id());

        return res;
    }

    Transaction *FindTransaction(txn_id_t txn_id) {
        if (txn_id == INVALID_TXN_ID) return nullptr;
        std::shared_lock<std::shared_mutex> lock(txn_map_mutex_);
        auto it = TransactionManager::txn_map.find(txn_id);
        return it == TransactionManager::txn_map.end() ? nullptr : it->second;
    }

    std::optional<RmRecord> GetVisibleTuple(int fd, const Rid &rid, const RmRecord &latest_physical,
                                            Transaction *reader);
    std::unique_ptr<RmRecord> ResolveVisibleTupleInReadGuard(RmFileHandle *fh, const Rid &rid,
                                                             std::unique_ptr<RmRecord> latest_physical,
                                                             Transaction *reader);
    std::unique_ptr<RmRecord> ReadVisibleTuple(RmFileHandle *fh, const Rid &rid,
                                               Context *context, Transaction *reader);
    WriteCheckOutcome CheckAndAcquireWrite(RmFileHandle *fh, const Rid &rid, Transaction *txn);
    WriteCheckOutcome AcquireWriteAndRecord(RmFileHandle *fh, const Rid &rid, const RmRecord &old_record,
                                            Transaction *txn, TupleWriteKind kind);
    void ReleaseWriteOwners(const std::vector<TupleKey> &keys, Transaction *txn);
    void RecordSerializablePredicateRead(int fd, const std::string &table_name,
                                         const std::vector<Condition> &conds,
                                         const std::vector<ColMeta> &cols,
                                         bool full_table_scan, bool lossy,
                                         Transaction *txn);
    void RecordSerializableTupleRead(int fd, const Rid &rid, Transaction *txn);
    void RecordSerializableWrite(int fd, const std::string &table_name, const Rid &rid,
                                 const std::optional<RmRecord> &old_record,
                                 const std::optional<RmRecord> &new_record,
                                 const std::vector<ColMeta> &cols,
                                 Transaction *txn);
    void RecordInsert(RmFileHandle *fh, const Rid &rid, Transaction *txn);
    void RecordUpdate(RmFileHandle *fh, const Rid &rid, const RmRecord &old_record, Transaction *txn);
    void RecordDelete(RmFileHandle *fh, const Rid &rid, const RmRecord &old_record, Transaction *txn);
    void RestoreTupleStateForStatement(const TupleKey &key, Transaction *txn);
    void RestoreOwnedTupleFlags(const std::vector<OwnedTupleFlagChange> &changes, Transaction *txn);
    StaleIndexEntry AddStaleIndexEntry(int fd, int index_no, const std::vector<char> &encoded_key,
                                       const Rid &rid, Transaction *txn);
    void RemoveStaleIndexEntries(const std::vector<StaleIndexEntry> &entries, Transaction *txn);
    std::vector<Rid> LookupStaleIndexEqual(int fd, int index_no, const std::vector<char> &encoded_key,
                                           Transaction *reader);
    std::vector<Rid> LookupStaleIndexRange(int fd, int index_no, const std::vector<char> &lower_key,
                                           const std::vector<char> &upper_key,
                                           const std::vector<ColType> &col_types,
                                           const std::vector<int> &col_lens, Transaction *reader);
    std::pair<std::vector<Rid>, bool> LookupStaleIndexRangeBounded(
        int fd, int index_no, const std::vector<char> &lower_key, const std::vector<char> &upper_key,
        const std::vector<ColType> &col_types, const std::vector<int> &col_lens,
        size_t max_results, Transaction *reader);
    std::uint64_t GetStaleIndexHandoffEpoch(int fd, int index_no) const;
    std::uint64_t GetUniqueKeyGeneration(int fd, int index_no, const std::vector<char> &encoded_key) const;
    bool ReserveUniqueKey(int fd, int index_no, const std::vector<char> &encoded_key, const Rid &rid,
                          Transaction *txn, const std::optional<Rid> &current_owner,
                          std::uint64_t observed_generation,
                          std::vector<UniqueKeyChange> *statement_changes);
    void CheckPendingKeyDeleteConflict(int fd, int index_no, const std::vector<char> &encoded_key,
                                       Transaction *txn);
    void MarkUniqueKeyDeleted(int fd, int index_no, const std::vector<char> &encoded_key, const Rid &rid,
                              Transaction *txn, std::vector<UniqueKeyChange> *statement_changes);
    void RestoreUniqueKeyChanges(const std::vector<UniqueKeyChange> &changes, Transaction *txn);
    void RecoverySetTupleState(RmFileHandle *fh, const Rid &rid, timestamp_t commit_ts, bool is_deleted);
    void RecoveryAdvanceCounters(txn_id_t next_txn_id, timestamp_t next_timestamp, timestamp_t last_commit_ts);
    // 静态检查点在静默态下调用：把已提交删除（逻辑删除）的记录从堆中物理清除，
    // 使检查点落盘的堆只包含存活记录，重启后从堆表重建索引才不会让删除记录复活。
    void VacuumCommittedDeletes();
    void PrintTupleStateStats() const;
    txn_id_t GetNextTxnId() const { return next_txn_id_.load(); }
    timestamp_t GetNextTimestamp() const { return next_timestamp_.load(); }
    timestamp_t GetLastCommitTs() const { return last_commit_ts_.load(); }
    bool HasActiveTransaction(txn_id_t ignore_txn_id = INVALID_TXN_ID);
    bool BeginStaticCheckpoint(size_t ignored_active_txn_count = 0);
    void EndStaticCheckpoint();

    // 事务结束后按 MVCC watermark 安全回收：把已提交/已中止且不再可能被任何活跃
    // 事务时间旅行读取到其 undo 版本（reclaim_ts <= watermark）的事务对象从
    // txn_map 中删除并释放，避免 txn_map 与 Transaction 对象随语句数无界增长
    // （否则 HasActiveTransaction 遍历会退化为 O(总事务数)，大规模检查点场景拖死服务）。
    void RetireTransaction(Transaction *txn, timestamp_t reclaim_ts);

    static std::unordered_map<txn_id_t, Transaction *> txn_map;     // 全局事务表，存放事务ID与事务对象的映射关系
    std::shared_mutex txn_map_mutex_;
    /** ------------------------以下函数仅可能在MVCC当中使用------------------------------------------*/

    /**
    * @brief 更新一个撤销链接，该链接将表堆元组与第一个撤销日志连接起来。
    * 在更新之前，将调用 `check` 函数以确保有效性。
    */
    bool UpdateUndoLink(Rid rid, std::optional<UndoLink> prev_link,
                        std::function<bool(std::optional<UndoLink>)> &&check = nullptr);

    /**
     * @brief 更新一个撤销链接，该链接将表堆元组与第一个撤销日志连接起来。
     * 在更新之前，将调用 `check` 函数以确保有效性。
     */
    bool UpdateVersionLink(Rid rid, std::optional<VersionUndoLink> prev_version,
                           std::function<bool(std::optional<VersionUndoLink>)> &&check = nullptr);

    /** @brief 获取表堆元组的第一个撤销日志。 */
    std::optional<UndoLink> GetUndoLink(Rid rid);

    /** @brief 获取表堆元组的第一个撤销日志。*/
    std::optional<VersionUndoLink> GetVersionLink(Rid rid);

    /** @brief 访问事务撤销日志缓冲区并获取撤销日志。如果事务不存在，返回 nullopt。
     * 如果索引超出范围仍然会抛出异常。 */
    std::optional<UndoLog> GetUndoLogOptional(UndoLink link);

    /** @brief 访问事务撤销日志缓冲区并获取撤销日志。除非访问当前事务缓冲区，
     * 否则应该始终调用此函数以获取撤销日志，而不是手动检索事务 shared_ptr 并访问缓冲区。 */
    UndoLog GetUndoLog(UndoLink link);

    /** @brief 获取系统中的最低读时间戳。 */
    timestamp_t GetWatermark();

    /** @brief 垃圾回收。仅在所有事务都未访问时调用。 */
    void GarbageCollection();

    struct PageVersionInfo {
        std::shared_mutex mutex_;
        /** 存储所有槽的先前版本信息。注意：不要使用 `[x]` 来访问它，因为
         * 即使不存在也会创建新元素。请使用 `find` 来代替。
         */
        std::unordered_map<slot_offset_t, VersionUndoLink> prev_version_;
    };

    /** 保护版本信息 */
    std::shared_mutex version_info_mutex_;
    /** 存储表堆中每个元组的先前版本。 */
    std::unordered_map<page_id_t, std::shared_ptr<PageVersionInfo>> version_info_;

    std::optional<TupleState> GetTupleState(const TupleKey &key);


private:
    ConcurrencyMode concurrency_mode_;      // 事务使用的并发控制算法，目前只需要考虑2PL
    std::atomic<txn_id_t> next_txn_id_{0};  // 用于分发事务ID
    std::atomic<timestamp_t> next_timestamp_{1};    // 仅用于分发提交时间戳
    // begin 只读取已完成全部内存发布的 commit watermark。并发 commit 可乱序刷 WAL，
    // 但必须按真实 timestamp reservation 顺序发布；兼容旧 checkpoint 留下的数值空洞。
    std::mutex commit_publication_mutex_;
    std::condition_variable commit_publication_cv_;
    std::map<timestamp_t, bool> commit_publications_;
    std::mutex latch_;  // 仅保护按 watermark 排序的事务退休队列
    // 已结束但暂未回收的事务：<reclaim_ts, txn_id>。按时间戳有序后，长快照期间
    // 每次提交只需检查可回收前缀，避免反复线性扫描形成 O(n^2)。
    std::multimap<timestamp_t, txn_id_t> reclaimable_txns_;
    struct RetiredTransaction {
        std::uint64_t retire_epoch{0};
        txn_id_t txn_id{INVALID_TXN_ID};
        Transaction *txn{nullptr};
    };
    std::atomic<std::uint64_t> version_gc_epoch_{1};
    std::mutex version_reader_slots_mutex_;
    std::vector<std::unique_ptr<VersionReaderSlot>> version_reader_slots_;
    std::mutex retired_txns_mutex_;
    // QSBR pending list. Entries newer than the oldest active VersionReadGuard
    // are retained; draining does not require all readers to become idle.
    std::vector<RetiredTransaction> pending_delete_txns_;
    std::atomic<size_t> pending_delete_txn_count_{0};
    std::mutex checkpoint_mutex_;
    std::condition_variable checkpoint_cv_;
    bool checkpoint_in_progress_{false};
    size_t active_txn_count_{0};
    SmManager *sm_manager_;
    LockManager *lock_manager_;

    std::atomic<timestamp_t> last_commit_ts_{0};    // 最后提交的时间戳,仅用于MVCC
    Watermark running_txns_{0};             // 存储所有正在运行事务的读取时间戳，以便于垃圾回收，仅用于MVCC

    static constexpr size_t kTupleStateShardCount = 64;
    static_assert((kTupleStateShardCount & (kTupleStateShardCount - 1)) == 0,
                  "tuple state shard count must be a power of two");

    struct TupleStateShard {
        mutable std::shared_mutex mutex;
        std::unordered_map<TupleKey, TupleState> states;
        std::atomic<size_t> approx_size{0};
        // heap 与 tuple-state 分两把锁发布；读者用此前后代数验证二者来自同一代。
        std::atomic<std::uint64_t> publication_epoch{0};
    };

    std::array<TupleStateShard, kTupleStateShardCount> tuple_state_shards_;

    // 静态检查点物理清除已提交逻辑删除记录的增量跟踪。
    // 未发生过检查点时（例如 without_checkpoint 场景）tracking 一直保持关闭，
    // 提交路径零额外开销；首次检查点做一次全量扫描后开启 tracking，之后每次
    // 检查点只需处理自上次以来新增的已提交删除键，避免 VacuumCommittedDeletes
    // 每个检查点都 O(全部 tuple_states_) 线性扫描造成检查点延迟随数据量增长。
    std::mutex vacuum_mutex_;
    bool vacuum_tracking_active_ = false;
    std::unordered_set<TupleKey> pending_committed_deletes_;

    static constexpr size_t kStaleIndexBucketCount = 64;
    static_assert((kStaleIndexBucketCount & (kStaleIndexBucketCount - 1)) == 0,
                  "stale index bucket count must be a power of two");

    struct StaleIndexBucket {
        mutable std::shared_mutex mutex;
        std::unordered_map<StaleIndexKey, std::vector<StaleIndexEntry>> equal_entries;
        std::unordered_map<std::uint64_t, std::map<std::string, std::vector<StaleIndexEntry>>> entries_by_index;
        // equal/range 两份镜像合计记作一个逻辑条目。
        std::atomic<size_t> entry_count{0};
        // abort 把候选来源从 stale registry 交回 B+ 树时递增。读路径仅在观察到
        // 并发 handoff 时重扫当前树，避免稳态点查无条件做第二次索引探测。
        std::atomic<std::uint64_t> handoff_epoch{0};
    };

    std::array<StaleIndexBucket, kStaleIndexBucketCount> stale_index_buckets_;
    std::atomic<std::uint64_t> stale_index_active_mask_{0};

    static constexpr size_t kUniqueKeyShardCount = 64;
    static_assert((kUniqueKeyShardCount & (kUniqueKeyShardCount - 1)) == 0,
                  "unique key shard count must be a power of two");

    struct UniqueKeyShard {
        std::mutex mutex;
        std::unordered_map<UniqueKeyId, UniqueKeyEntry> entries;
        // B+ 点查与 reservation 之间的乐观验证序号。提交/回滚改变逻辑 owner 时
        // 递增，使过时的树观察重新探测；稳态注册表只保存活跃写，不随数据量增长。
        std::atomic<std::uint64_t> generation{0};
    };

    std::array<UniqueKeyShard, kUniqueKeyShardCount> unique_key_shards_;
    static constexpr size_t kUniqueRegistryCompactBucketThreshold = 8192;
    std::atomic<bool> unique_registry_compaction_needed_{false};

    using TupleStateCommitGroups = std::array<std::vector<size_t>, kTupleStateShardCount>;
    using StaleIndexCommitGroups = std::array<std::vector<size_t>, kStaleIndexBucketCount>;
    using UniqueKeyCommitGroups = std::array<std::vector<size_t>, kUniqueKeyShardCount>;

    struct PredicateRead {
        txn_id_t reader{INVALID_TXN_ID};
        int fd{INVALID_PAGE_ID};
        std::string table_name;
        std::vector<Condition> conds;
        std::vector<ColMeta> cols;
        bool full_table_scan{false};
        bool lossy{false};
        timestamp_t reader_start_ts{0};
    };

    struct WriteIntent {
        txn_id_t writer{INVALID_TXN_ID};
        int fd{INVALID_PAGE_ID};
        std::string table_name;
        Rid rid{};
        std::optional<RmRecord> old_record;
        std::optional<RmRecord> new_record;
        timestamp_t writer_start_ts{0};
        timestamp_t writer_commit_ts{INVALID_TS};
        bool committed{false};
    };

    struct SerializableTxnInterval {
        timestamp_t start_ts{0};
        timestamp_t finish_ts{INVALID_TS};
        TransactionState state{TransactionState::GROWING};
    };

    std::mutex ssi_mutex_;
    // SERIALIZABLE 提交从预检查到 durable Mark 逐个冻结 SSI 图；性能评测使用 SI，
    // 因而不会把该正确性锁引入计分热路径。
    std::mutex serializable_commit_mutex_;
    std::atomic<bool> ssi_metadata_present_{false};
    std::unordered_map<txn_id_t, SerializableTxnInterval> serializable_intervals_;
    std::unordered_map<txn_id_t, std::unordered_set<TupleKey>> serializable_rid_reads_;
    std::unordered_map<txn_id_t, std::vector<PredicateRead>> serializable_predicate_reads_;
    std::unordered_map<txn_id_t, std::vector<WriteIntent>> serializable_write_intents_;
    std::unordered_map<TupleKey, std::unordered_set<txn_id_t>> serializable_rid_read_index_;
    std::unordered_map<int, std::unordered_set<txn_id_t>> serializable_predicate_read_fd_index_;
    std::unordered_map<int, std::unordered_set<txn_id_t>> serializable_write_intent_fd_index_;
    std::set<std::pair<txn_id_t, txn_id_t>> serializable_rw_edges_;
    std::unordered_map<txn_id_t, std::unordered_set<txn_id_t>> serializable_rw_out_edges_;
    std::unordered_map<txn_id_t, std::unordered_set<txn_id_t>> serializable_rw_in_edges_;

    void RegisterSerializableBegin(Transaction *txn);
    std::unique_lock<std::mutex> PrepareSerializableCommit(Transaction *txn, timestamp_t commit_ts);
    void MarkSerializableCommitLocked(Transaction *txn);
    void CleanupSerializableAbort(txn_id_t txn_id);
    void GarbageCollectSerializableMetadata();
    bool AddSerializableRwEdgeAndCheck(txn_id_t from_reader, txn_id_t to_writer, txn_id_t current_txn);
    bool SerializableDangerousStructureLocked(txn_id_t tin, txn_id_t tpivot, txn_id_t tout,
                                              timestamp_t assumed_tout_finish = INVALID_TS) const;
    bool SerializablePredicateMatches(const PredicateRead &predicate, const RmRecord &record) const;
    bool SerializableCondsMatch(const std::vector<ColMeta> &cols, const std::vector<Condition> &conds,
                                const RmRecord &record) const;
    bool SerializableWriteAffectsPredicate(const PredicateRead &predicate, const WriteIntent &intent) const;
    bool SerializableWriteChangesRecord(const WriteIntent &intent) const;
    bool SerializableWriteVisibleToReader(const WriteIntent &intent, timestamp_t reader_start_ts) const;
    static std::uint64_t StaleIndexRegistryId(int fd, int index_no);
    static size_t StaleIndexBucketIndex(int fd, int index_no);
    static std::uint64_t TupleStateShardHash(const TupleKey &key);
    static size_t TupleStateShardIndex(const TupleKey &key);
    std::uint64_t GetTuplePublicationEpoch(const TupleKey &key) const;
    TupleStateShard &ShardFor(const TupleKey &key);
    const TupleStateShard &ShardFor(const TupleKey &key) const;
    void AssignTupleStateLocked(TupleStateShard &shard, const TupleKey &key, const TupleState &state);
    bool EraseTupleStateLocked(TupleStateShard &shard, const TupleKey &key);
    void ObserveTupleStateShardSize(size_t size);
    StaleIndexBucket &StaleIndexBucketFor(int fd, int index_no);
    const StaleIndexBucket &StaleIndexBucketFor(int fd, int index_no) const;
    void RemoveSerializableTxnIndexesLocked(txn_id_t txn_id);
    void RemoveSerializableEdgesForTxnLocked(txn_id_t txn_id);
    void CommitStaleIndexEntries(Transaction *txn, timestamp_t commit_ts,
                                 const std::vector<StaleIndexEntry> &entries,
                                 const StaleIndexCommitGroups &groups);
    void CleanupStaleIndexAbort(Transaction *txn);
    void GarbageCollectStaleIndex();
    bool StaleIndexEntryVisibleToReader(const StaleIndexEntry &entry, Transaction *reader) const;
    static size_t UniqueKeyShardIndex(const UniqueKeyId &key);
    static size_t UniqueKeyShardIndex(int fd, int index_no, const char *encoded_key, size_t key_size);
    UniqueKeyShard &UniqueKeyShardFor(const UniqueKeyId &key);
    void CaptureUniqueKeyStatementChangeLocked(const UniqueKeyId &key, UniqueKeyShard &shard, Transaction *txn,
                                               std::vector<UniqueKeyChange> *statement_changes);
    TxnUniqueKeyWriteState GetOrCreateTxnUniqueStateLocked(const UniqueKeyId &key,
                                                           const UniqueKeyEntry &entry,
                                                           bool had_entry, Transaction *txn);
    void PrepareUniqueKeyCommit(Transaction *txn, const std::vector<TxnUniqueKeyWriteState> &states,
                                const UniqueKeyCommitGroups &groups);
    void CommitUniqueKeyEntries(Transaction *txn, const std::vector<TxnUniqueKeyWriteState> &states,
                                const UniqueKeyCommitGroups &groups);
    void CleanupUniqueKeyAbort(Transaction *txn);
    void MaybeCompactUniqueKeyRegistry();
    timestamp_t ReserveCommitTimestamp();
    void CancelCommitTimestamp(timestamp_t commit_ts);
    std::unique_lock<std::mutex> AwaitCommitPublicationTurn(timestamp_t commit_ts);
    void CompleteCommitPublication(timestamp_t commit_ts, std::unique_lock<std::mutex> &publication_lock);
    void FinishActiveTransactionForCheckpoint();
    VersionReaderSlot *GetVersionReaderSlot();
    void GarbageCollectTupleStatesForRetired(const std::vector<RetiredTransaction> &retired,
                                             timestamp_t watermark);
    void TryDrainRetiredTransactions();
    std::unique_ptr<RmRecord> ResolveVisibleTupleImpl(int fd, RmFileHandle *fh, const Rid &rid,
                                                      std::unique_ptr<RmRecord> latest_physical,
                                                      Transaction *reader);
};
