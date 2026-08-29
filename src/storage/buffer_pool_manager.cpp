/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "buffer_pool_manager.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>
#include <vector>

size_t BufferPoolManager::floor_power_of_two(size_t value) {
    if (value == 0) {
        return 0;
    }
    size_t power = 1;
    while ((power << 1U) <= value) {
        power <<= 1U;
    }
    return power;
}

size_t BufferPoolManager::partition_of(const PageId &page_id) const {
    size_t hash = static_cast<size_t>(PageIdHash{}(page_id));
    return hash & (num_partitions_ - 1);
}

bool BufferPoolManager::find_victim_page(size_t partition_idx, Partition &part, frame_id_t *frame_id) {
    if (!part.free_list.empty()) {
        *frame_id = part.free_list.front();
        part.free_list.pop_front();
        part.clean_available_count.fetch_sub(1, std::memory_order_acq_rel);
        return true;
    }

    constexpr size_t kCleanVictimScan = 64;
    if (part.replacer->victim_if(frame_id,
                                 [&](frame_id_t candidate) {
                                     return !pages_[candidate].is_dirty_ &&
                                            !frame_io_in_progress_[candidate] &&
                                            !frame_writeback_in_progress_[candidate];
                                 },
                                 kCleanVictimScan)) {
        Page *page = &pages_[*frame_id];
        page->is_evictable_ = false;
        part.clean_available_count.fetch_sub(1, std::memory_order_acq_rel);
        return true;
    }
    if (!part.replacer->victim(frame_id)) {
        return false;
    }
    Page *page = &pages_[*frame_id];
    page->is_evictable_ = false;
    if (!page->is_dirty_) {
        part.clean_available_count.fetch_sub(1, std::memory_order_acq_rel);
    } else {
        sync_dirty_evictions_.fetch_add(1, std::memory_order_relaxed);
        notify_partition_pressure(partition_idx);
    }
    return true;
}

void BufferPoolManager::mark_frame_pinned(Partition &part, Page *page, frame_id_t frame_id) {
    if (page->is_evictable_) {
        part.replacer->pin(frame_id);
        page->is_evictable_ = false;
        if (!page->is_dirty_) {
            size_t clean = part.clean_available_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (clean < part.clean_reserve && part.dirty_count.load(std::memory_order_acquire) > 0 &&
                page->id_.page_no != INVALID_PAGE_ID) {
                notify_partition_pressure(partition_of(page->id_));
            }
        }
    }
}

void BufferPoolManager::mark_frame_evictable(Partition &part, Page *page, frame_id_t frame_id,
                                              bool keep_cold) {
    if (page->pin_count_ != 0 || page->is_evictable_ || frame_io_in_progress_[frame_id] ||
        frame_writeback_in_progress_[frame_id]) {
        return;
    }
    if (keep_cold) {
        part.replacer->unpin_cold(frame_id);
    } else {
        part.replacer->unpin(frame_id);
    }
    page->is_evictable_ = true;
    if (!page->is_dirty_) {
        size_t clean = part.clean_available_count.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (clean >= part.clean_target && page->id_.page_no != INVALID_PAGE_ID) {
            urgent_partition_mask_.fetch_and(~(std::uint64_t{1} << partition_of(page->id_)),
                                             std::memory_order_acq_rel);
        }
    }
}

void BufferPoolManager::return_free_frame(Partition &part, frame_id_t frame_id) {
    pages_[frame_id].is_evictable_ = false;
    part.free_list.push_back(frame_id);
    part.clean_available_count.fetch_add(1, std::memory_order_acq_rel);
}

void BufferPoolManager::register_dirty_page(Partition &part, Page *page) {
    if (page == nullptr || page->id_.page_no == INVALID_PAGE_ID) {
        return;
    }
    if (page->get_wal_dirty_state() == Page::WalDirtyState::kClean) {
        // 普通 DML 必须先 set_page_lsn；load/rebuild 必须显式
        // mark_wal_free_dirty。未声明来源的 dirty 在写回时硬失败。
        page->wal_dirty_state_.store(Page::WalDirtyState::kUnknown, std::memory_order_release);
    }
    const bool was_dirty = page->is_dirty_;
    ++page->dirty_generation_;
    page->is_dirty_ = true;
    if (!was_dirty) {
        bool inserted = part.dirty_pages.insert(page->id_).second;
        assert(inserted);
        if (!inserted) {
            return;
        }
        size_t part_count = part.dirty_count.fetch_add(1, std::memory_order_acq_rel) + 1;
        size_t count = dirty_page_count_.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (part_count > part.dirty_high_watermark) {
            dirty_pressure_mask_.fetch_or(std::uint64_t{1} << partition_of(page->id_),
                                          std::memory_order_acq_rel);
        }
        if (page_cleaner_started_.load(std::memory_order_acquire) &&
            (count > page_cleaner_dirty_threshold_.load(std::memory_order_acquire) ||
             (adaptive_cleaner_enabled_.load(std::memory_order_acquire) &&
              part_count > part.dirty_high_watermark))) {
            page_cleaner_cv_.notify_one();
        }
    }
}

bool BufferPoolManager::erase_dirty_page(Partition &part, const PageId &page_id) {
    if (part.dirty_pages.erase(page_id) == 0) {
        return false;
    }
    size_t part_count = part.dirty_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    const size_t partition_idx = partition_of(page_id);
    if (part_count <= part.dirty_low_watermark) {
        dirty_pressure_mask_.fetch_and(~(std::uint64_t{1} << partition_idx), std::memory_order_acq_rel);
    }
    if (part_count == 0) {
        urgent_partition_mask_.fetch_and(~(std::uint64_t{1} << partition_idx), std::memory_order_acq_rel);
    }
    dirty_page_count_.fetch_sub(1, std::memory_order_acq_rel);
    return true;
}

bool BufferPoolManager::partition_needs_cleaning(size_t partition_idx) const {
    if (partition_idx >= partitions_.size()) {
        return false;
    }
    const auto &part = partitions_[partition_idx];
    const auto bit = std::uint64_t{1} << partition_idx;
    if (((urgent_partition_mask_.load(std::memory_order_acquire) |
          dirty_pressure_mask_.load(std::memory_order_acquire)) & bit) != 0) {
        return true;
    }
    if (!adaptive_cleaner_enabled_.load(std::memory_order_acquire)) {
        return false;
    }
    return part.dirty_count.load(std::memory_order_acquire) > part.dirty_high_watermark ||
           (part.dirty_count.load(std::memory_order_acquire) > 0 &&
            part.clean_available_count.load(std::memory_order_acquire) < part.clean_reserve);
}

void BufferPoolManager::notify_partition_pressure(size_t partition_idx) {
    if (partition_idx >= 64) {
        return;
    }
    urgent_partition_mask_.fetch_or(std::uint64_t{1} << partition_idx, std::memory_order_acq_rel);
    cleaner_pressure_wakeups_.fetch_add(1, std::memory_order_relaxed);
    if (page_cleaner_started_.load(std::memory_order_acquire)) {
        page_cleaner_cv_.notify_one();
    }
}

bool BufferPoolManager::prepare_writeback_locked(size_t partition_idx, frame_id_t frame_id,
                                                 bool require_dirty, bool require_unpinned,
                                                 WritebackTask *task) {
    if (task == nullptr || partition_idx >= partitions_.size() || frame_id < 0 ||
        static_cast<size_t>(frame_id) >= pool_size_) {
        return false;
    }
    auto &part = partitions_[partition_idx];
    Page *page = &pages_[frame_id];
    if (page->id_.page_no == INVALID_PAGE_ID || frame_io_in_progress_[frame_id] ||
        frame_writeback_in_progress_[frame_id] || (require_dirty && !page->is_dirty_) ||
        (require_unpinned && page->pin_count_ != page->resident_pin_count_)) {
        return false;
    }

    // 临时 pin 保证锁外 WAL/pwrite 期间 frame 不会被淘汰或重用。
    ++page->pin_count_;
    mark_frame_pinned(part, page, frame_id);
    frame_writeback_in_progress_[frame_id] = true;
    frame_writeback_snapshot_ready_[frame_id] = false;
    task->partition_idx = partition_idx;
    task->frame_id = frame_id;
    task->page_id = page->id_;
    task->dirty_generation = page->dirty_generation_;
    task->was_dirty = page->is_dirty_;
    task->page_lsn = INVALID_LSN;
    task->wal_state = Page::WalDirtyState::kClean;
    task->data.reset();
    return true;
}

void BufferPoolManager::mark_snapshot_ready(const WritebackTask &task) {
    auto &part = partitions_[task.partition_idx];
    {
        std::scoped_lock lock{part.latch};
        if (frame_writeback_in_progress_[task.frame_id] && pages_[task.frame_id].id_ == task.page_id) {
            frame_writeback_snapshot_ready_[task.frame_id] = true;
        }
    }
    part.writeback_cv.notify_all();
}

void BufferPoolManager::capture_writeback_snapshot(WritebackTask *task) {
    if (task == nullptr || task->frame_id < 0 || static_cast<size_t>(task->frame_id) >= pool_size_) {
        throw InternalError("Invalid writeback task");
    }
    Page *page = &pages_[task->frame_id];
    std::array<char, PAGE_SIZE> snapshot{};
    {
        PageReadGuard page_guard(page);
        task->page_lsn = page->get_page_lsn();
        task->wal_state = page->get_wal_dirty_state();
        memcpy(snapshot.data(), page->data_, PAGE_SIZE);
    }
    task->data = std::make_unique<std::array<char, PAGE_SIZE>>(std::move(snapshot));
    mark_snapshot_ready(*task);
}

bool BufferPoolManager::finish_writeback(const WritebackTask &task, bool write_succeeded) {
    if (task.partition_idx >= partitions_.size() || task.frame_id < 0 ||
        static_cast<size_t>(task.frame_id) >= pool_size_) {
        return false;
    }
    auto &part = partitions_[task.partition_idx];
    bool cleaned = false;
    {
        std::scoped_lock lock{part.latch};
        Page *page = &pages_[task.frame_id];
        if (frame_writeback_in_progress_[task.frame_id]) {
            // 只有写回自己的 pin 且脏代次未变时，该快照才代表当前页。
            // 若并发写者已取得页，即使它尚未 unpin 发布新代次，也保守地
            // 保留 dirty，避免丢失它的后续修改。
            if (write_succeeded && task.was_dirty && page->id_ == task.page_id && page->is_dirty_ &&
                page->dirty_generation_ == task.dirty_generation &&
                page->pin_count_ == page->resident_pin_count_ + 1) {
                page->is_dirty_ = false;
                page->page_lsn_.store(INVALID_LSN, std::memory_order_release);
                page->wal_dirty_state_.store(Page::WalDirtyState::kClean, std::memory_order_release);
                erase_dirty_page(part, page->id_);
                cleaned = true;
            }
            if (page->pin_count_ > 0) {
                --page->pin_count_;
            }
            frame_writeback_in_progress_[task.frame_id] = false;
            frame_writeback_snapshot_ready_[task.frame_id] = false;
            // 没有并发访问且已成功清理时，它仍是 cleaner 选中的冷页；
            // 放回 LRU 尾部，clean reserve 才能成为前台真正可见的 victim。
            mark_frame_evictable(part, page, task.frame_id, cleaned);
        }
    }
    part.writeback_cv.notify_all();
    return cleaned;
}

std::vector<lsn_t> BufferPoolManager::validate_writeback_tasks(
    const std::vector<WritebackTask> &tasks) const {
    std::vector<lsn_t> page_lsns;
    page_lsns.reserve(tasks.size());
    for (const auto &task : tasks) {
        if (!task.was_dirty && task.wal_state == Page::WalDirtyState::kClean) {
            continue;
        }
        if (task.wal_state == Page::WalDirtyState::kUnknown ||
            task.wal_state == Page::WalDirtyState::kClean) {
            throw InternalError("Dirty page has undeclared WAL provenance " + task.page_id.toString());
        }
        if (task.wal_state == Page::WalDirtyState::kWalBacked) {
            if (task.page_lsn == INVALID_LSN) {
                throw InternalError("WAL-backed dirty page has no page_lsn " + task.page_id.toString());
            }
            page_lsns.push_back(task.page_lsn);
        }
    }
    return page_lsns;
}

void BufferPoolManager::flush_wal_for_tasks(const std::vector<WritebackTask> &tasks) {
    std::vector<lsn_t> page_lsns = validate_writeback_tasks(tasks);
    if (page_lsns.empty()) {
        return;
    }
    if (flush_page_lsns_) {
        flush_page_lsns_(page_lsns);
        return;
    }
    if (!has_log_record_ || !flush_log_up_to_) {
        throw InternalError("WAL barrier is not configured");
    }
    lsn_t max_lsn = INVALID_LSN;
    for (lsn_t page_lsn : page_lsns) {
        if (!has_log_record_(page_lsn)) {
            throw InternalError("Unknown WAL page_lsn " + std::to_string(page_lsn));
        }
        max_lsn = max_lsn == INVALID_LSN ? page_lsn : std::max(max_lsn, page_lsn);
    }
    flush_log_up_to_(max_lsn);
}

BufferPoolManager::WritebackBatchResult BufferPoolManager::perform_writeback_batch(
    std::vector<WritebackTask> *tasks, bool skip_wal_barrier) {
    WritebackBatchResult result;
    if (tasks == nullptr || tasks->empty()) {
        return result;
    }
    try {
        for (auto &task : *tasks) {
            capture_writeback_snapshot(&task);
        }
        if (!skip_wal_barrier) {
            flush_wal_for_tasks(*tasks);
            cleaner_wal_batches_.fetch_add(1, std::memory_order_relaxed);
        } else {
            // checkpoint 已整体刷完 WAL 时可省掉 LogManager 往返，但不能
            // 让 fast path 绕过脏页来源校验。
            (void)validate_writeback_tasks(*tasks);
        }
    } catch (...) {
        for (auto &task : *tasks) {
            finish_writeback(task, false);
        }
        throw;
    }

    std::sort(tasks->begin(), tasks->end(), [](const WritebackTask &lhs, const WritebackTask &rhs) {
        return lhs.page_id < rhs.page_id;
    });
    for (size_t i = 0; i < tasks->size(); ++i) {
        auto &task = (*tasks)[i];
        try {
            disk_manager_->write_page(task.page_id.fd, task.page_id.page_no, task.data->data(), PAGE_SIZE);
            ++result.written;
            if (finish_writeback(task, true)) {
                ++result.cleaned;
            }
        } catch (...) {
            finish_writeback(task, false);
            for (size_t j = i + 1; j < tasks->size(); ++j) {
                finish_writeback((*tasks)[j], false);
            }
            throw;
        }
    }
    return result;
}

bool BufferPoolManager::perform_writeback(WritebackTask *task, bool skip_wal_barrier) {
    if (task == nullptr) {
        return false;
    }
    std::vector<WritebackTask> tasks;
    try {
        tasks.reserve(1);
        tasks.push_back(std::move(*task));
    } catch (...) {
        finish_writeback(*task, false);
        throw;
    }
    return perform_writeback_batch(&tasks, skip_wal_barrier).cleaned == 1;
}

bool BufferPoolManager::writeback_page(PageId page_id, bool require_dirty, bool skip_wal_barrier) {
    auto partition_idx = partition_of(page_id);
    auto &part = partitions_[partition_idx];
    WritebackTask task;
    for (;;) {
        std::unique_lock<std::mutex> lock{part.latch};
        auto it = part.page_table.find(page_id);
        if (it == part.page_table.end()) {
            if (part.inflight_page_loads.count(page_id) != 0) {
                part.io_cv.wait(lock, [&]() { return part.inflight_page_loads.count(page_id) == 0; });
                continue;
            }
            return false;
        }
        frame_id_t frame_id = it->second;
        if (frame_io_in_progress_[frame_id]) {
            part.io_cv.wait(lock, [&]() { return !frame_io_in_progress_[frame_id]; });
            continue;
        }
        if (frame_writeback_in_progress_[frame_id]) {
            part.writeback_cv.wait(lock, [&]() { return !frame_writeback_in_progress_[frame_id]; });
            continue;
        }
        if (!prepare_writeback_locked(partition_idx, frame_id, require_dirty, false, &task)) {
            return !require_dirty;
        }
        break;
    }
    perform_writeback(&task, skip_wal_barrier);
    return true;
}

bool BufferPoolManager::prepare_one_dirty_page(
    size_t partition_idx, const std::vector<PageId> &attempted_pages,
    WritebackTask *task) {
    if (partition_idx >= partitions_.size() || task == nullptr) {
        return false;
    }
    auto &part = partitions_[partition_idx];
    std::scoped_lock lock{part.latch};

    constexpr size_t kColdCandidateScan = 128;
    for (frame_id_t frame_id : part.replacer->cold_candidates(kColdCandidateScan)) {
        Page *page = &pages_[frame_id];
        if (!page->is_dirty_ ||
            std::find(attempted_pages.begin(), attempted_pages.end(), page->id_) != attempted_pages.end()) {
            continue;
        }
        if (prepare_writeback_locked(partition_idx, frame_id, true, true, task)) {
            return true;
        }
    }

    // 冷 LRU 候选在正常压力下应已足够；dirty set 仅作稀有兜底，
    // 避免为每次热页重脏维护第二套 list+hash 元数据。
    for (auto it = part.dirty_pages.begin(); it != part.dirty_pages.end();) {
        PageId page_id = *it;
        ++it;
        if (std::find(attempted_pages.begin(), attempted_pages.end(), page_id) != attempted_pages.end()) {
            continue;
        }
        auto page_it = part.page_table.find(page_id);
        if (page_it == part.page_table.end() || !pages_[page_it->second].is_dirty_) {
            erase_dirty_page(part, page_id);
            continue;
        }
        if (prepare_writeback_locked(partition_idx, page_it->second, true, true, task)) {
            return true;
        }
    }
    return false;
}

void BufferPoolManager::update_page(Partition &part, Page *page, PageId new_page_id, frame_id_t new_frame_id) {
    // 调用者必须先在锁外完成脏页写回；这里只修改分区元数据。
    assert(!page->is_dirty_);
    if (page->id_.page_no != INVALID_PAGE_ID) {
        erase_dirty_page(part, page->id_);
        part.page_table.erase(page->id_);
    }
    page->reset_memory();
    page->id_ = new_page_id;
    page->pin_count_ = 0;
    page->resident_pin_count_ = 0;
    page->is_dirty_ = false;
    page->dirty_generation_ = 0;
    page->is_evictable_ = false;
    if (new_page_id.page_no != INVALID_PAGE_ID) {
        part.page_table[new_page_id] = new_frame_id;
    }
}

Page *BufferPoolManager::fetch_page(PageId page_id) {
    auto part_idx = partition_of(page_id);
    auto &part = partitions_[part_idx];
    for (;;) {
        std::unique_lock<std::mutex> lock{part.latch};
        auto it = part.page_table.find(page_id);
        if (it != part.page_table.end()) {
            frame_id_t frame_id = it->second;
            if (frame_io_in_progress_[frame_id]) {
                part.io_cv.wait(lock, [&]() { return !frame_io_in_progress_[frame_id]; });
                continue;
            }
            if (frame_writeback_in_progress_[frame_id] &&
                !frame_writeback_snapshot_ready_[frame_id]) {
                part.writeback_cv.wait(lock, [&]() {
                    return !frame_writeback_in_progress_[frame_id] ||
                           frame_writeback_snapshot_ready_[frame_id];
                });
                continue;
            }
            Page *page = &pages_[frame_id];
            page->pin_count_++;
            mark_frame_pinned(part, page, frame_id);
            return page;
        }
        if (part.inflight_page_loads.count(page_id) != 0) {
            part.io_cv.wait(lock, [&]() { return part.inflight_page_loads.count(page_id) == 0; });
            continue;
        }

        frame_id_t frame_id;
        if (!find_victim_page(part_idx, part, &frame_id)) {
            return nullptr;
        }

        Page *frame_page = &pages_[frame_id];
        const PageId victim_page_id = frame_page->id_;
        WritebackTask evict_task;
        if (victim_page_id.page_no != INVALID_PAGE_ID && frame_page->is_dirty_) {
            evict_task.partition_idx = part_idx;
            evict_task.frame_id = frame_id;
            evict_task.page_id = victim_page_id;
            evict_task.was_dirty = true;
        }
        // victim 的映射和内容保留到 pwrite 成功。并发读取旧 PageId 会在
        // frame_io 上等待；相同新 PageId 的 miss 则在 inflight 集合上等待。
        frame_io_in_progress_[frame_id] = true;
        part.inflight_page_loads.insert(page_id);
        lock.unlock();

        std::vector<char> read_buffer(PAGE_SIZE, 0);
        try {
            if (evict_task.was_dirty) {
                evict_task.data = std::make_unique<std::array<char, PAGE_SIZE>>();
                {
                    PageReadGuard page_guard(frame_page);
                    evict_task.page_lsn = frame_page->get_page_lsn();
                    evict_task.wal_state = frame_page->get_wal_dirty_state();
                    memcpy(evict_task.data->data(), frame_page->data_, PAGE_SIZE);
                }
                std::vector<WritebackTask> barrier_tasks;
                barrier_tasks.push_back(std::move(evict_task));
                flush_wal_for_tasks(barrier_tasks);
                auto &snapshot = barrier_tasks.front();
                disk_manager_->write_page(snapshot.page_id.fd, snapshot.page_id.page_no,
                                          snapshot.data->data(), PAGE_SIZE);
            }
            disk_manager_->read_page(page_id.fd, page_id.page_no, read_buffer.data(), PAGE_SIZE);
        } catch (...) {
            std::unique_lock<std::mutex> rollback_lock{part.latch};
            part.inflight_page_loads.erase(page_id);
            frame_io_in_progress_[frame_id] = false;
            if (victim_page_id.page_no == INVALID_PAGE_ID) {
                return_free_frame(part, frame_id);
            } else {
                // victim() 已将 frame 从 replacer 移除；失败时恢复为可淘汰。
                mark_frame_evictable(part, frame_page, frame_id);
            }
            part.io_cv.notify_all();
            throw;
        }

        std::unique_lock<std::mutex> finish_lock{part.latch};
        if (victim_page_id.page_no != INVALID_PAGE_ID) {
            if (frame_page->is_dirty_) {
                frame_page->is_dirty_ = false;
                erase_dirty_page(part, victim_page_id);
            }
            part.page_table.erase(victim_page_id);
        }
        frame_page->reset_memory();
        memcpy(frame_page->data_, read_buffer.data(), PAGE_SIZE);
        frame_page->id_ = page_id;
        frame_page->pin_count_ = 1;
        frame_page->resident_pin_count_ = 0;
        frame_page->is_dirty_ = false;
        frame_page->dirty_generation_ = 0;
        frame_page->is_evictable_ = false;
        part.page_table[page_id] = frame_id;
        part.inflight_page_loads.erase(page_id);
        frame_io_in_progress_[frame_id] = false;
        part.io_cv.notify_all();
        return frame_page;
    }
}

Page *BufferPoolManager::fetch_resident_page(PageId page_id) {
    Page *page = fetch_page(page_id);
    if (page == nullptr) {
        return nullptr;
    }

    auto &part = partitions_[partition_of(page_id)];
    bool converted = false;
    {
        std::scoped_lock lock{part.latch};
        if (page->id_ == page_id && page->pin_count_ > page->resident_pin_count_) {
            ++page->resident_pin_count_;
            converted = true;
        }
    }
    if (!converted) {
        // fetch_page 已经交付一个普通 pin；转换失败时必须成对释放。
        unpin_page(page_id, false);
        return nullptr;
    }
    return page;
}

bool BufferPoolManager::unpin_page(PageId page_id, bool is_dirty) {
    auto &part = partitions_[partition_of(page_id)];
    std::scoped_lock lock{part.latch};

    auto it = part.page_table.find(page_id);
    if (it == part.page_table.end()) {
        return false;
    }
    frame_id_t frame_id = it->second;
    Page *page = &pages_[frame_id];
    if (page->pin_count_ <= page->resident_pin_count_) {
        return false;
    }

    page->pin_count_--;
    if (is_dirty) {
        register_dirty_page(part, page);
    }
    mark_frame_evictable(part, page, frame_id);
    return true;
}

bool BufferPoolManager::unpin_page_impl(Page *page, bool is_dirty, bool release_resident_pin) {
    if (page == nullptr) {
        return false;
    }

    // 不对外部指针做指针减法；先验证地址确实落在连续 frame 数组内。
    const auto begin = reinterpret_cast<std::uintptr_t>(pages_);
    const auto end = begin + sizeof(Page) * pool_size_;
    const auto address = reinterpret_cast<std::uintptr_t>(page);
    if (address < begin || address >= end || (address - begin) % sizeof(Page) != 0) {
        return false;
    }
    const frame_id_t frame_id = static_cast<frame_id_t>((address - begin) / sizeof(Page));
    const PageId page_id = page->id_;
    if (page_id.page_no == INVALID_PAGE_ID) {
        return false;
    }

    auto &part = partitions_[partition_of(page_id)];
    std::scoped_lock lock{part.latch};
    if (frame_id < part.frame_begin || frame_id >= part.frame_end || &pages_[frame_id] != page ||
        !(page->id_ == page_id) || page->pin_count_ <= 0) {
        return false;
    }
    if (release_resident_pin) {
        if (page->resident_pin_count_ <= 0) {
            return false;
        }
        --page->resident_pin_count_;
    } else if (page->pin_count_ <= page->resident_pin_count_) {
        // 普通 unpin 不得误释放文件句柄保留的基础 pin。
        return false;
    }

    --page->pin_count_;
    if (is_dirty) {
        register_dirty_page(part, page);
    }
    mark_frame_evictable(part, page, frame_id);
    return true;
}

bool BufferPoolManager::unpin_page(Page *page, bool is_dirty) {
    return unpin_page_impl(page, is_dirty, false);
}

bool BufferPoolManager::release_resident_page(Page *page, bool is_dirty) {
    return unpin_page_impl(page, is_dirty, true);
}

bool BufferPoolManager::flush_page(PageId page_id) {
    return writeback_page(page_id, false, false);
}

Page *BufferPoolManager::new_page(PageId *page_id) {
    // Lock order for M9:
    // 1) allocate_page (DiskManager internal atomic/alloc lock)
    // 2) partition latch
    // Never nest the reverse order.
    for (size_t attempts = 0; attempts < num_partitions_; ++attempts) {
        page_id->page_no = disk_manager_->allocate_page(page_id->fd);
        auto part_idx = partition_of(*page_id);
        auto &part = partitions_[part_idx];
        std::unique_lock<std::mutex> lock{part.latch};

        frame_id_t frame_id;
        if (!find_victim_page(part_idx, part, &frame_id)) {
            continue;
        }
        Page *page = &pages_[frame_id];
        const PageId victim_page_id = page->id_;
        WritebackTask evict_task;
        if (victim_page_id.page_no != INVALID_PAGE_ID && page->is_dirty_) {
            evict_task.partition_idx = part_idx;
            evict_task.frame_id = frame_id;
            evict_task.page_id = victim_page_id;
            evict_task.was_dirty = true;
        }
        // 与 fetch_page 的淘汰协议一致：旧页写回成功前保留映射和内存副本。
        frame_io_in_progress_[frame_id] = true;
        part.inflight_page_loads.insert(*page_id);
        lock.unlock();

        try {
            if (evict_task.was_dirty) {
                evict_task.data = std::make_unique<std::array<char, PAGE_SIZE>>();
                {
                    PageReadGuard page_guard(page);
                    evict_task.page_lsn = page->get_page_lsn();
                    evict_task.wal_state = page->get_wal_dirty_state();
                    memcpy(evict_task.data->data(), page->data_, PAGE_SIZE);
                }
                std::vector<WritebackTask> barrier_tasks;
                barrier_tasks.push_back(std::move(evict_task));
                flush_wal_for_tasks(barrier_tasks);
                auto &snapshot = barrier_tasks.front();
                disk_manager_->write_page(snapshot.page_id.fd, snapshot.page_id.page_no,
                                          snapshot.data->data(), PAGE_SIZE);
            }
        } catch (...) {
            std::unique_lock<std::mutex> rollback_lock{part.latch};
            part.inflight_page_loads.erase(*page_id);
            frame_io_in_progress_[frame_id] = false;
            if (victim_page_id.page_no == INVALID_PAGE_ID) {
                return_free_frame(part, frame_id);
            } else {
                mark_frame_evictable(part, page, frame_id);
            }
            part.io_cv.notify_all();
            throw;
        }

        std::unique_lock<std::mutex> finish_lock{part.latch};
        if (victim_page_id.page_no != INVALID_PAGE_ID) {
            if (page->is_dirty_) {
                page->is_dirty_ = false;
                erase_dirty_page(part, victim_page_id);
            }
            part.page_table.erase(victim_page_id);
        }
        page->reset_memory();
        page->id_ = *page_id;
        page->pin_count_ = 1;
        page->resident_pin_count_ = 0;
        page->is_dirty_ = false;
        page->dirty_generation_ = 0;
        page->is_evictable_ = false;
        part.page_table[*page_id] = frame_id;
        part.inflight_page_loads.erase(*page_id);
        frame_io_in_progress_[frame_id] = false;
        part.io_cv.notify_all();
        return page;
    }
    return nullptr;
}

bool BufferPoolManager::delete_page(PageId page_id) {
    auto partition_idx = partition_of(page_id);
    auto &part = partitions_[partition_idx];
    for (;;) {
        std::unique_lock<std::mutex> lock{part.latch};
        auto it = part.page_table.find(page_id);
        if (it == part.page_table.end()) {
            if (part.inflight_page_loads.count(page_id) != 0) {
                part.io_cv.wait(lock, [&]() { return part.inflight_page_loads.count(page_id) == 0; });
                continue;
            }
            return true;
        }
        frame_id_t frame_id = it->second;
        if (frame_io_in_progress_[frame_id]) {
            part.io_cv.wait(lock, [&]() { return !frame_io_in_progress_[frame_id]; });
            continue;
        }
        if (frame_writeback_in_progress_[frame_id]) {
            part.writeback_cv.wait(lock, [&]() { return !frame_writeback_in_progress_[frame_id]; });
            continue;
        }
        Page *page = &pages_[frame_id];
        if (page->pin_count_ != 0) {
            return false;
        }
        if (page->is_dirty_) {
            lock.unlock();
            writeback_page(page_id, true, false);
            continue;
        }

        mark_frame_pinned(part, page, frame_id);
        update_page(part, page, {page_id.fd, INVALID_PAGE_ID}, frame_id);
        return_free_frame(part, frame_id);
        return true;
    }
}

void BufferPoolManager::flush_all_pages(int fd) {
    std::vector<PageId> page_ids;
    for (auto &part : partitions_) {
        std::scoped_lock lock{part.latch};
        for (const auto &[page_id, frame_id] : part.page_table) {
            if (page_id.fd == fd) page_ids.push_back(page_id);
        }
    }
    for (const auto &page_id : page_ids) writeback_page(page_id, false, false);
}

bool BufferPoolManager::discard_all_pages(int fd) {
    std::vector<PageId> page_ids;
    // 关闭文件要求静默态。先完整检查，再开始逐出，避免发现后置 pinned
    // 页时已经删除了前面的 frame，给调用方留下半清理状态。
    for (auto &part : partitions_) {
        std::scoped_lock lock{part.latch};
        for (const auto &[page_id, frame_id] : part.page_table) {
            if (page_id.fd == fd) {
                if (pages_[frame_id].pin_count_ != 0) {
                    return false;
                }
                page_ids.push_back(page_id);
            }
        }
    }
    for (const PageId &page_id : page_ids) {
        if (!delete_page(page_id)) {
            return false;
        }
    }
    return true;
}

void BufferPoolManager::flush_dirty_pages(int fd) {
    std::vector<PageId> page_ids;
    for (auto &part : partitions_) {
        std::scoped_lock lock{part.latch};
        for (const auto &page_id : part.dirty_pages) {
            if (page_id.fd == fd) page_ids.push_back(page_id);
        }
    }
    for (const auto &page_id : page_ids) writeback_page(page_id, true, false);
}

void BufferPoolManager::flush_all_dirty_pages() {
    std::vector<PageId> page_ids;
    for (auto &part : partitions_) {
        std::scoped_lock lock{part.latch};
        page_ids.insert(page_ids.end(), part.dirty_pages.begin(), part.dirty_pages.end());
    }
    for (const auto &page_id : page_ids) writeback_page(page_id, true, true);
}

size_t BufferPoolManager::dirty_page_count_no_lock() {
    return dirty_page_count_.load(std::memory_order_acquire);
}

BufferPoolManager::WritebackBatchResult BufferPoolManager::flush_dirty_batch(
    size_t max_pages, size_t dirty_threshold, bool skip_wal_barrier, bool pressure_only) {
    WritebackBatchResult result;
    if (max_pages == 0 || num_partitions_ == 0) {
        return result;
    }
    if (!pressure_only && dirty_page_count_no_lock() <= dirty_threshold) {
        return result;
    }

    std::vector<WritebackTask> tasks;
    tasks.reserve(max_pages);
    std::vector<PageId> attempted_pages;
    attempted_pages.reserve(max_pages);
    size_t start = flush_round_robin_cursor_.fetch_add(1, std::memory_order_relaxed) % num_partitions_;
    try {
        bool progressed = true;
        while (tasks.size() < max_pages && progressed) {
            progressed = false;
            for (size_t i = 0; i < num_partitions_ && tasks.size() < max_pages; ++i) {
                size_t idx = (start + i) % num_partitions_;
                if (pressure_only && !partition_needs_cleaning(idx)) {
                    continue;
                }
                WritebackTask task;
                bool prepared = false;
                try {
                    prepared = prepare_one_dirty_page(idx, attempted_pages, &task);
                    if (prepared) {
                        attempted_pages.push_back(task.page_id);
                        tasks.push_back(std::move(task));
                        prepared = false;
                        progressed = true;
                    }
                } catch (...) {
                    if (prepared) {
                        finish_writeback(task, false);
                    }
                    throw;
                }
            }
            start = (start + 1) % num_partitions_;
        }
    } catch (...) {
        for (auto &prepared : tasks) {
            finish_writeback(prepared, false);
        }
        throw;
    }
    if (tasks.empty()) {
        return result;
    }

    result = perform_writeback_batch(&tasks, skip_wal_barrier);
    for (size_t idx = 0; idx < num_partitions_; ++idx) {
        auto &part = partitions_[idx];
        if (part.dirty_count.load(std::memory_order_acquire) <= part.dirty_low_watermark) {
            dirty_pressure_mask_.fetch_and(~(std::uint64_t{1} << idx), std::memory_order_acq_rel);
        }
        if (part.dirty_count.load(std::memory_order_acquire) == 0 ||
            part.clean_available_count.load(std::memory_order_acquire) >= part.clean_target) {
            urgent_partition_mask_.fetch_and(~(std::uint64_t{1} << idx), std::memory_order_acq_rel);
        }
    }
    return result;
}

size_t BufferPoolManager::flush_some_dirty_pages_impl(size_t max_pages, size_t dirty_threshold,
                                                      bool skip_wal_barrier, size_t *cleaned_pages) {
    auto result = flush_dirty_batch(max_pages, dirty_threshold, skip_wal_barrier, false);
    if (cleaned_pages != nullptr) {
        *cleaned_pages = result.cleaned;
    }
    return result.written;
}

size_t BufferPoolManager::flush_some_dirty_pages_after_wal(size_t max_pages, size_t dirty_threshold) {
    return flush_some_dirty_pages_impl(max_pages, dirty_threshold, true);
}

size_t BufferPoolManager::flush_some_dirty_pages(size_t max_pages, size_t dirty_threshold) {
    return flush_some_dirty_pages_impl(max_pages, dirty_threshold, false);
}

size_t BufferPoolManager::dirty_page_count() {
    return dirty_page_count_no_lock();
}

void BufferPoolManager::StartPageCleaner(size_t batch_pages, size_t dirty_threshold, bool adaptive) {
    std::lock_guard<std::mutex> lock(page_cleaner_mutex_);
    if (page_cleaner_started_.load(std::memory_order_acquire)) {
        return;
    }
    page_cleaner_batch_pages_ = batch_pages;
    page_cleaner_dirty_threshold_.store(dirty_threshold, std::memory_order_release);
    adaptive_cleaner_enabled_.store(adaptive, std::memory_order_release);
    stop_page_cleaner_ = false;
    page_cleaner_thread_ = std::thread(&BufferPoolManager::PageCleanerMain, this);
    page_cleaner_started_.store(true, std::memory_order_release);
    bool pressure = dirty_page_count_no_lock() > dirty_threshold;
    for (size_t idx = 0; adaptive && idx < num_partitions_ && !pressure; ++idx) {
        pressure = partition_needs_cleaning(idx);
    }
    if (pressure) page_cleaner_cv_.notify_one();
}

void BufferPoolManager::NudgePageCleaner() {
    if (!page_cleaner_started_.load(std::memory_order_acquire)) {
        return;
    }
    bool pressure = dirty_page_count_no_lock() >
                    page_cleaner_dirty_threshold_.load(std::memory_order_acquire);
    for (size_t idx = 0; adaptive_cleaner_enabled_.load(std::memory_order_acquire) &&
                         idx < num_partitions_ && !pressure;
         ++idx) {
        pressure = partition_needs_cleaning(idx);
    }
    if (pressure) page_cleaner_cv_.notify_one();
}

void BufferPoolManager::StopPageCleaner() {
    {
        std::lock_guard<std::mutex> lock(page_cleaner_mutex_);
        if (!page_cleaner_started_.load(std::memory_order_acquire)) {
            return;
        }
        stop_page_cleaner_ = true;
    }
    page_cleaner_cv_.notify_one();
    if (page_cleaner_thread_.joinable()) {
        page_cleaner_thread_.join();
    }
    std::lock_guard<std::mutex> lock(page_cleaner_mutex_);
    page_cleaner_started_.store(false, std::memory_order_release);
    stop_page_cleaner_ = false;
}

void BufferPoolManager::PageCleanerMain() {
    std::unique_lock<std::mutex> lock(page_cleaner_mutex_);
    auto retry_delay = std::chrono::milliseconds(1);
    while (!stop_page_cleaner_) {
        page_cleaner_cv_.wait(lock, [&]() {
            if (stop_page_cleaner_ ||
                dirty_page_count_no_lock() >
                    page_cleaner_dirty_threshold_.load(std::memory_order_acquire)) {
                return true;
            }
            if (!adaptive_cleaner_enabled_.load(std::memory_order_acquire)) {
                return false;
            }
            for (size_t idx = 0; idx < num_partitions_; ++idx) {
                if (partition_needs_cleaning(idx)) return true;
            }
            return false;
        });
        if (stop_page_cleaner_) {
            break;
        }

        const size_t batch_pages = page_cleaner_batch_pages_;
        const size_t dirty_threshold = page_cleaner_dirty_threshold_.load(std::memory_order_acquire);
        lock.unlock();
        WritebackBatchResult result;
        try {
            const bool pressure_only = dirty_page_count_no_lock() <= dirty_threshold;
            result = flush_dirty_batch(batch_pages, dirty_threshold, false, pressure_only);
            cleaner_pages_written_.fetch_add(result.written, std::memory_order_relaxed);
            cleaner_pages_cleaned_.fetch_add(result.cleaned, std::memory_order_relaxed);
        } catch (...) {
            // 后台清理失败不改变事务提交语义；checkpoint/close 仍会执行完整刷页并暴露持久错误。
        }
        lock.lock();
        if (result.cleaned == 0 && !stop_page_cleaner_) {
            // 候选页可能全部被 pin、被并发重脏，或底层 I/O 暂时失败。
            // 采用有上限的退避，避免高水位谓词持续为真时形成错误风暴。
            page_cleaner_cv_.wait_for(lock, retry_delay);
            retry_delay = std::min(retry_delay * 2, std::chrono::milliseconds(100));
        } else {
            retry_delay = std::chrono::milliseconds(1);
        }
    }
}

std::string BufferPoolManager::writeback_stats() const {
    std::ostringstream out;
    out << "writeback sync_dirty_evictions=" << sync_dirty_evictions_.load(std::memory_order_relaxed)
        << " cleaner_written=" << cleaner_pages_written_.load(std::memory_order_relaxed)
        << " cleaner_cleaned=" << cleaner_pages_cleaned_.load(std::memory_order_relaxed)
        << " wal_batches=" << cleaner_wal_batches_.load(std::memory_order_relaxed)
        << " pressure_wakeups=" << cleaner_pressure_wakeups_.load(std::memory_order_relaxed)
        << " dirty_now=" << dirty_page_count_.load(std::memory_order_relaxed);
    return out.str();
}

lsn_t BufferPoolManager::oldest_dirty_lsn() {
    lsn_t oldest = INVALID_LSN;
    for (auto &part : partitions_) {
        std::scoped_lock lock{part.latch};
        std::vector<PageId> stale;
        for (const auto &page_id : part.dirty_pages) {
            auto page_it = part.page_table.find(page_id);
            if (page_it == part.page_table.end()) {
                stale.push_back(page_id);
                continue;
            }
            Page *page = &pages_[page_it->second];
            if (!page->is_dirty_) {
                stale.push_back(page_id);
                continue;
            }
            // sidecar 是原子元数据；持分区锁已固定 frame 身份，无需再按
            // partition -> page 的逆序获取页锁。
            lsn_t page_lsn = page->get_page_lsn();
            if (page->get_wal_dirty_state() != Page::WalDirtyState::kWalBacked ||
                page_lsn == INVALID_LSN) {
                return 0;
            }
            if (oldest == INVALID_LSN || page_lsn < oldest) {
                oldest = page_lsn;
            }
        }
        for (const auto &page_id : stale) {
            erase_dirty_page(part, page_id);
        }
    }
    return oldest;
}
