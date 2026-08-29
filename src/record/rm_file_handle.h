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

#include <assert.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "bitmap.h"
#include "common/context.h"
#include "recovery/log_manager.h"
#include "rm_defs.h"

class RmManager;

/* 对表数据文件中的页面进行封装 */
struct RmPageHandle {
    const RmFileHdr *file_hdr;  // 当前页面所在文件的文件头指针
    Page *page;                 // 页面的实际数据，包括页面存储的数据、元信息等
    RmPageHdr *page_hdr;        // page->data的第一部分，存储页面元信息，指针指向首地址，长度为sizeof(RmPageHdr)
    char *bitmap;               // page->data的第二部分，存储页面的bitmap，指针指向首地址，长度为file_hdr->bitmap_size
    char *slots;                // page->data的第三部分，存储表的记录，指针指向首地址，每个slot的长度为file_hdr->record_size

    RmPageHandle(const RmFileHdr *fhdr_, Page *page_) : file_hdr(fhdr_), page(page_) {
        page_hdr = reinterpret_cast<RmPageHdr *>(page->get_data() + page->OFFSET_PAGE_HDR);
        bitmap = page->get_data() + sizeof(RmPageHdr) + page->OFFSET_PAGE_HDR;
        slots = bitmap + file_hdr->bitmap_size;
    }

    // 返回指定slot_no的slot存储收地址
    char* get_slot(int slot_no) const {
        return slots + slot_no * file_hdr->record_size;  // slots的首地址 + slot个数 * 每个slot的大小(每个record的大小)
    }
};

/* 每个RmFileHandle对应一个表的数据文件，里面有多个page，每个page的数据封装在RmPageHandle中 */
class RmFileHandle {
    friend class RmScan;    
    friend class RmManager;

   private:
    DiskManager *disk_manager_;
    BufferPoolManager *buffer_pool_manager_;
    int fd_;        // 打开文件后产生的文件句柄
    RmFileHdr file_hdr_;    // 文件头，维护当前表文件的元数据
    std::atomic<int> num_pages_snapshot_{0};
    // 单调标志：一旦该文件发布过 MVCC tuple state 就不再回落，允许读路径安全跳过空状态表。
    std::atomic<bool> has_mvcc_state_{false};
    std::unordered_map<int, std::unordered_set<int>> quarantined_slots_;
    // 预约只存在于内存中：它在 heap bitmap 发布前保证并发 INSERT 不会获得同一 RID。
    std::unordered_map<int, std::unordered_set<int>> reserved_slots_;
    std::vector<int> active_insert_pages_;
    size_t active_insert_cursor_{0};
    size_t reserved_slot_count_{0};
    mutable std::mutex heap_mutex_;
    std::mutex insert_mutex_;

   public:
    class InsertSlotReservation {
        friend class RmFileHandle;

       public:
        InsertSlotReservation() = default;
        ~InsertSlotReservation();
        InsertSlotReservation(InsertSlotReservation &&other) noexcept;
        InsertSlotReservation &operator=(InsertSlotReservation &&other) noexcept;
        InsertSlotReservation(const InsertSlotReservation &) = delete;
        InsertSlotReservation &operator=(const InsertSlotReservation &) = delete;

        const Rid &rid() const { return rid_; }
        bool valid() const { return owner_ != nullptr && active_; }

       private:
        InsertSlotReservation(RmFileHandle *owner, Rid rid) : owner_(owner), rid_(rid), active_(true) {}
        void Reset() noexcept;

        RmFileHandle *owner_{nullptr};
        Rid rid_{};
        bool active_{false};
    };

    RmFileHandle(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, int fd)
        : disk_manager_(disk_manager), buffer_pool_manager_(buffer_pool_manager), fd_(fd) {
        // 注意：这里从磁盘中读出文件描述符为fd的文件的file_hdr，读到内存中
        // 这里实际就是初始化file_hdr，只不过是从磁盘中读出进行初始化
        // init file_hdr_
        disk_manager_->read_page(fd, RM_FILE_HDR_PAGE, (char *)&file_hdr_, sizeof(file_hdr_));
        num_pages_snapshot_.store(file_hdr_.num_pages, std::memory_order_release);
        // disk_manager管理的fd对应的文件中，设置从file_hdr_.num_pages开始分配page_no
        disk_manager_->set_fd2pageno(fd, file_hdr_.num_pages);
    }

    RmFileHdr get_file_hdr() {
        std::lock_guard<std::mutex> heap_lock(heap_mutex_);
        return file_hdr_;
    }
    int GetFd() { return fd_; }
    void flush_file_hdr() {
        std::lock_guard<std::mutex> heap_lock(heap_mutex_);
        disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE, (char *)&file_hdr_, sizeof(file_hdr_));
    }
    int num_pages_snapshot() const { return num_pages_snapshot_.load(std::memory_order_acquire); }
    void MarkHasMvccState() { has_mvcc_state_.store(true, std::memory_order_release); }
    bool HasMvccState() const { return has_mvcc_state_.load(std::memory_order_acquire); }
    std::unique_lock<std::mutex> LockInsertMutex() { return std::unique_lock<std::mutex>(insert_mutex_); }
    static bool ParallelInsertEnabled();

    /* 判断指定位置上是否已经存在一条记录，通过Bitmap来判断 */
    bool is_record(const Rid &rid) const {
        RmPageHandle page_handle = fetch_page_handle(rid.page_no);
        bool result = false;
        {
            PageReadGuard page_guard(page_handle.page);
            result = Bitmap::is_set(page_handle.bitmap, rid.slot_no);
        }
        buffer_pool_manager_->unpin_page(page_handle.page, false);
        return result;
    }

    std::unique_ptr<RmRecord> get_record(const Rid &rid, Context *context) const;

    Rid prepare_insert_rid();

    InsertSlotReservation reserve_insert_slot();

    Rid insert_record(char *buf, Context *context, const std::string &table_name = "");

    // LOAD fast path: keep one heap page pinned and fill all of its free slots
    // before moving to the next page.  This is generic heap-file machinery and
    // intentionally knows nothing about CSV schemas or benchmark tables.
    template <typename FillRecordFn>
    size_t bulk_insert_records(FillRecordFn &&fill_record) {
        std::lock_guard<std::mutex> heap_lock(heap_mutex_);
        std::vector<char> record(static_cast<size_t>(file_hdr_.record_size));
        Page *page = nullptr;
        std::unique_ptr<PageWriteGuard> page_guard;
        RmPageHdr *page_hdr = nullptr;
        char *bitmap = nullptr;
        char *slots = nullptr;
        int next_slot = file_hdr_.num_records_per_page;

        auto release_page = [&]() {
            if (page != nullptr) {
                page->mark_wal_free_dirty();
                page_guard.reset();
                buffer_pool_manager_->unpin_page(page, true);
                page = nullptr;
            }
        };
        auto pin_free_page = [&]() {
            RmPageHandle handle = create_page_handle();
            page = handle.page;
            page_hdr = handle.page_hdr;
            bitmap = handle.bitmap;
            slots = handle.slots;
            page_guard = std::make_unique<PageWriteGuard>(page);
            next_slot = first_available_slot(handle);
        };
        auto find_next_slot = [&](int current) {
            int slot = Bitmap::next_bit(false, bitmap, file_hdr_.num_records_per_page, current);
            auto quarantine_it = quarantined_slots_.find(page->get_page_id().page_no);
            while (slot < file_hdr_.num_records_per_page && quarantine_it != quarantined_slots_.end() &&
                   quarantine_it->second.count(slot) > 0) {
                slot = Bitmap::next_bit(false, bitmap, file_hdr_.num_records_per_page, slot);
            }
            return slot;
        };

        size_t inserted = 0;
        try {
            while (fill_record(record.data())) {
                if (page == nullptr) pin_free_page();
                assert(next_slot < file_hdr_.num_records_per_page);
                memcpy(slots + next_slot * file_hdr_.record_size, record.data(), file_hdr_.record_size);
                Bitmap::set(bitmap, next_slot);
                ++page_hdr->num_records;
                ++inserted;

                next_slot = find_next_slot(next_slot);
                if (next_slot >= file_hdr_.num_records_per_page) {
                    RmPageHandle handle(&file_hdr_, page);
                    unlink_free_page_if_needed(handle, INVALID_LSN);
                    release_page();
                }
            }
            release_page();
        } catch (...) {
            release_page();
            throw;
        }
        return inserted;
    }

    void insert_record(const Rid &rid, char *buf, lsn_t page_lsn = INVALID_LSN);
    lsn_t insert_record(const Rid &rid, char *buf, Context *context, const std::string &table_name);
    lsn_t insert_record(InsertSlotReservation &reservation, char *buf, Context *context,
                        const std::string &table_name);

    void delete_record(const Rid &rid, Context *context, const std::string &table_name = "");
    void delete_record_quarantine(const Rid &rid, lsn_t page_lsn = INVALID_LSN);
    lsn_t log_delete_record(const Rid &rid, const RmRecord &old_record, Context *context,
                            const std::string &table_name);

    void update_record(const Rid &rid, char *buf, Context *context, const std::string &table_name = "",
                       const RmRecord *old_record = nullptr, lsn_t page_lsn = INVALID_LSN);
    lsn_t log_update_record(const Rid &rid, const RmRecord &old_record, const RmRecord &new_record,
                            Context *context, const std::string &table_name);

    RmPageHandle create_new_page_handle();

    RmPageHandle fetch_page_handle(int page_no) const;

   private:
    // Heap metadata helpers below are called by public mutators while heap_mutex_ is held.
    RmPageHandle create_new_page_handle_unlocked();
    RmPageHandle create_page_handle();

    void release_page_handle(RmPageHandle &page_handle);
    lsn_t append_heap_log(LogRecord &record, Context *context);
    void publish_num_pages_snapshot_unlocked() {
        num_pages_snapshot_.store(file_hdr_.num_pages, std::memory_order_release);
    }
    int first_available_slot(const RmPageHandle &page_handle) const;
    bool page_has_available_slot(const RmPageHandle &page_handle) const;
    void unlink_free_page_if_needed(RmPageHandle &page_handle, lsn_t page_lsn = INVALID_LSN);
    void cancel_insert_reservation(InsertSlotReservation *reservation) noexcept;
    void complete_insert_reservation(InsertSlotReservation *reservation, bool page_became_full,
                                     lsn_t page_lsn);
    void ensure_active_insert_pages_unlocked(size_t desired_pages);
    void add_active_insert_page_unlocked(int page_no);
    void remove_active_insert_page_unlocked(int page_no);
};
