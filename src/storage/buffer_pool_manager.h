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
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <thread>

#include "disk_manager.h"
#include "errors.h"
#include "page.h"
#include "replacer/lru_replacer.h"
#include "replacer/replacer.h"

class BufferPoolManager {
   private:
    struct Partition {
        std::mutex latch;
        std::condition_variable io_cv;
        std::condition_variable writeback_cv;
        std::unordered_map<PageId, frame_id_t, PageIdHash> page_table;
        std::unordered_set<PageId, PageIdHash> dirty_pages;
        // 页尚未进入 page_table，但已有线程负责装载。相同 PageId 的 miss
        // 必须等待，避免为同一磁盘页创建两个缓存副本。
        std::unordered_set<PageId, PageIdHash> inflight_page_loads;
        std::list<frame_id_t> free_list;
        std::unique_ptr<LRUReplacer> replacer;
        frame_id_t frame_begin{0};
        frame_id_t frame_end{0};
        std::atomic<size_t> dirty_count{0};
        // free frame 与 clean+unpinned+evictable frame 的总数。
        std::atomic<size_t> clean_available_count{0};
        size_t dirty_high_watermark{1};
        size_t dirty_low_watermark{0};
        size_t clean_reserve{1};
        size_t clean_target{1};
    };

    struct WritebackTask {
        size_t partition_idx{0};
        frame_id_t frame_id{INVALID_FRAME_ID};
        PageId page_id{.fd = -1, .page_no = INVALID_PAGE_ID};
        std::uint64_t dirty_generation{0};
        bool was_dirty{false};
        lsn_t page_lsn{INVALID_LSN};
        Page::WalDirtyState wal_state{Page::WalDirtyState::kClean};
        std::unique_ptr<std::array<char, PAGE_SIZE>> data;
    };

    struct WritebackBatchResult {
        size_t written{0};
        size_t cleaned{0};
    };

    size_t pool_size_;      // buffer_pool中可容纳页面的个数，即帧的个数
    Page *pages_;           // buffer_pool中的Page对象数组，在构造空间中申请内存空间，在析构函数中释放，大小为BUFFER_POOL_SIZE
    std::deque<Partition> partitions_;
    size_t num_partitions_{1};
    std::atomic<size_t> flush_round_robin_cursor_{0};
    DiskManager *disk_manager_;
    std::function<bool(lsn_t)> has_log_record_{};
    std::function<void(lsn_t)> flush_log_up_to_{};
    std::function<void(const std::vector<lsn_t> &)> flush_page_lsns_{};
    // Legacy latch kept for compatibility with existing unit tests that inspect internals.
    // M9 partitioned BPM does not use this lock for page metadata synchronization.
    std::mutex latch_;
    // 不使用 vector<bool>：不同分区并发修改相邻 bit 会竞争同一存储字。
    std::vector<std::uint8_t> frame_io_in_progress_;
    std::vector<std::uint8_t> frame_writeback_in_progress_;
    std::vector<std::uint8_t> frame_writeback_snapshot_ready_;
    std::atomic<size_t> dirty_page_count_{0};
    std::atomic<std::uint64_t> urgent_partition_mask_{0};
    std::atomic<std::uint64_t> dirty_pressure_mask_{0};
    std::thread page_cleaner_thread_;
    std::mutex page_cleaner_mutex_;
    std::condition_variable page_cleaner_cv_;
    std::atomic<bool> page_cleaner_started_{false};
    bool stop_page_cleaner_{false};
    size_t page_cleaner_batch_pages_{0};
    std::atomic<size_t> page_cleaner_dirty_threshold_{0};
    std::atomic<bool> adaptive_cleaner_enabled_{true};

    std::atomic<std::uint64_t> sync_dirty_evictions_{0};
    std::atomic<std::uint64_t> cleaner_pages_written_{0};
    std::atomic<std::uint64_t> cleaner_pages_cleaned_{0};
    std::atomic<std::uint64_t> cleaner_wal_batches_{0};
    std::atomic<std::uint64_t> cleaner_pressure_wakeups_{0};

    size_t partition_of(const PageId &page_id) const;
    bool find_victim_page(size_t partition_idx, Partition &part, frame_id_t *frame_id);
    void mark_frame_pinned(Partition &part, Page *page, frame_id_t frame_id);
    void mark_frame_evictable(Partition &part, Page *page, frame_id_t frame_id,
                              bool keep_cold = false);
    void return_free_frame(Partition &part, frame_id_t frame_id);
    void update_page(Partition &part, Page *page, PageId new_page_id, frame_id_t new_frame_id);
    bool unpin_page_impl(Page *page, bool is_dirty, bool release_resident_pin);
    void register_dirty_page(Partition &part, Page *page);
    bool erase_dirty_page(Partition &part, const PageId &page_id);
    bool partition_needs_cleaning(size_t partition_idx) const;
    void notify_partition_pressure(size_t partition_idx);
    bool prepare_writeback_locked(size_t partition_idx, frame_id_t frame_id, bool require_dirty,
                                  bool require_unpinned, WritebackTask *task);
    void mark_snapshot_ready(const WritebackTask &task);
    void capture_writeback_snapshot(WritebackTask *task);
    bool finish_writeback(const WritebackTask &task, bool write_succeeded);
    bool perform_writeback(WritebackTask *task, bool skip_wal_barrier);
    WritebackBatchResult perform_writeback_batch(std::vector<WritebackTask> *tasks,
                                                 bool skip_wal_barrier);
    std::vector<lsn_t> validate_writeback_tasks(const std::vector<WritebackTask> &tasks) const;
    void flush_wal_for_tasks(const std::vector<WritebackTask> &tasks);
    bool writeback_page(PageId page_id, bool require_dirty, bool skip_wal_barrier);
    bool prepare_one_dirty_page(size_t partition_idx,
                                const std::vector<PageId> &attempted_pages,
                                WritebackTask *task);
    WritebackBatchResult flush_dirty_batch(size_t max_pages, size_t dirty_threshold,
                                           bool skip_wal_barrier, bool pressure_only);
    size_t flush_some_dirty_pages_impl(size_t max_pages, size_t dirty_threshold,
                                       bool skip_wal_barrier, size_t *cleaned_pages = nullptr);
    size_t dirty_page_count_no_lock();
    static size_t floor_power_of_two(size_t value);
    void PageCleanerMain();

   public:
    BufferPoolManager(size_t pool_size, DiskManager *disk_manager)
        : pool_size_(pool_size), disk_manager_(disk_manager) {
        // 为buffer pool分配一块连续的内存空间
        pages_ = new Page[pool_size_];
        frame_io_in_progress_.assign(pool_size_, false);
        frame_writeback_in_progress_.assign(pool_size_, false);
        frame_writeback_snapshot_ready_.assign(pool_size_, false);
        constexpr size_t kConfiguredPartitions = BPM_PARTITION_COUNT;
        constexpr size_t kMinFramesPerPartition = 8;
        static_assert((kConfiguredPartitions & (kConfiguredPartitions - 1)) == 0,
                      "BPM_PARTITION_COUNT must be a power of two");
        num_partitions_ = floor_power_of_two(std::min(pool_size_, kConfiguredPartitions));
        while (num_partitions_ > 1 && (pool_size_ / num_partitions_) < kMinFramesPerPartition) {
            num_partitions_ >>= 1U;
        }
        if (num_partitions_ == 0) {
            num_partitions_ = 1;
        }
        partitions_.resize(num_partitions_);

        const size_t base_frames = pool_size_ / num_partitions_;
        const size_t remainder = pool_size_ % num_partitions_;
        frame_id_t frame_cursor = 0;
        for (size_t i = 0; i < num_partitions_; ++i) {
            size_t part_frames = base_frames + (i < remainder ? 1 : 0);
            auto &part = partitions_[i];
            part.frame_begin = frame_cursor;
            part.frame_end = static_cast<frame_id_t>(frame_cursor + part_frames);
            part.replacer = std::make_unique<LRUReplacer>(part_frames);
            part.clean_available_count.store(part_frames, std::memory_order_relaxed);
            // 常态优先让写热点留在内存。单分区 dirty 接近容量上限时按
            // 水位清理；更早的保护由真实 clean victim reserve 触发。
            part.dirty_high_watermark = std::max<size_t>(2, (part_frames * 3) / 4);
            part.dirty_high_watermark = std::min(part.dirty_high_watermark,
                                                 part_frames > 1 ? part_frames - 1 : size_t{1});
            part.dirty_low_watermark = part_frames / 2;
            part.clean_reserve = std::max<size_t>(1, part_frames / 64);
            part.clean_target = std::min(part_frames,
                                         std::max(part.clean_reserve, part.clean_reserve * 2));
            for (frame_id_t fid = part.frame_begin; fid < part.frame_end; ++fid) {
                part.free_list.emplace_back(fid);
            }
            frame_cursor = part.frame_end;
        }
        assert(frame_cursor == static_cast<frame_id_t>(pool_size_));
    }

    ~BufferPoolManager() {
        StopPageCleaner();
        delete[] pages_;
    }

    void set_wal_barrier(std::function<bool(lsn_t)> has_log_record,
                         std::function<void(lsn_t)> flush_log_up_to,
                         std::function<void(const std::vector<lsn_t> &)> flush_page_lsns = {}) {
        has_log_record_ = std::move(has_log_record);
        flush_log_up_to_ = std::move(flush_log_up_to);
        flush_page_lsns_ = std::move(flush_page_lsns);
    }
    void flush_log_up_to_lsn(lsn_t page_lsn) {
        if (page_lsn == INVALID_LSN || !has_log_record_ || !flush_log_up_to_) {
            return;
        }
        if (!has_log_record_(page_lsn)) {
            throw InternalError("Unknown WAL page_lsn " + std::to_string(page_lsn));
        }
        flush_log_up_to_(page_lsn);
    }

   public:
    Page* fetch_page(PageId page_id);

    // 为文件句柄生命周期内反复访问的结构页保留一个基础 pin。resident pin
    // 不绕过页锁、WAL 或写回，只避免每次树遍历都重新查 page_table。
    Page *fetch_resident_page(PageId page_id);

    bool unpin_page(PageId page_id, bool is_dirty);

    // 已持有 Page* 的存储热路径可直接定位 frame，省去第二次 page_table 哈希。
    // PageId 版本继续保留给兼容接口和只保存页号的调用方。
    bool unpin_page(Page *page, bool is_dirty);

    bool release_resident_page(Page *page, bool is_dirty = false);

    bool flush_page(PageId page_id);

    Page* new_page(PageId* page_id);

    bool delete_page(PageId page_id);

    void flush_all_pages(int fd);
    // 文件关闭前在静默态逐出该 fd 的全部 frame，防止 OS 复用整数 fd 后
    // 新文件误命中旧 PageId。调用方必须先 flush，并保证没有并发访问/pin。
    bool discard_all_pages(int fd);
    void flush_dirty_pages(int fd);
    // Single-pass flush of all dirty pages across all files.
    // Caller must guarantee all WAL log records are already flushed to disk
    // before calling this (no per-page WAL barrier is checked here).
    void flush_all_dirty_pages();
    size_t flush_some_dirty_pages_after_wal(size_t max_pages, size_t dirty_threshold = 0);
    size_t flush_some_dirty_pages(size_t max_pages, size_t dirty_threshold = 0);
    size_t dirty_page_count();
    void StartPageCleaner(size_t batch_pages, size_t dirty_threshold, bool adaptive = true);
    void NudgePageCleaner();
    void StopPageCleaner();
    lsn_t oldest_dirty_lsn();
    size_t pool_size() const { return pool_size_; }
    std::string writeback_stats() const;
};
