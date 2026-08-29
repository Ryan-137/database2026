/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "rm_file_handle.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {

constexpr size_t kMaxActiveInsertPages = 16;

void MarkPageWalState(Page *page, lsn_t page_lsn) {
    if (page_lsn != INVALID_LSN) {
        page->set_page_lsn(page_lsn);
    } else {
        page->mark_wal_free_dirty();
    }
}

}  // namespace

RmFileHandle::InsertSlotReservation::~InsertSlotReservation() {
    Reset();
}

RmFileHandle::InsertSlotReservation::InsertSlotReservation(InsertSlotReservation &&other) noexcept
    : owner_(other.owner_), rid_(other.rid_), active_(other.active_) {
    other.owner_ = nullptr;
    other.active_ = false;
}

RmFileHandle::InsertSlotReservation &RmFileHandle::InsertSlotReservation::operator=(
    InsertSlotReservation &&other) noexcept {
    if (this != &other) {
        Reset();
        owner_ = other.owner_;
        rid_ = other.rid_;
        active_ = other.active_;
        other.owner_ = nullptr;
        other.active_ = false;
    }
    return *this;
}

void RmFileHandle::InsertSlotReservation::Reset() noexcept {
    if (owner_ != nullptr && active_) {
        owner_->cancel_insert_reservation(this);
    }
    owner_ = nullptr;
    active_ = false;
}

bool RmFileHandle::ParallelInsertEnabled() {
    static const bool enabled = [] {
        const char *value = std::getenv("RMDB_ENABLE_PARALLEL_HEAP_INSERT");
        return value == nullptr || std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

/**
 * @description: 获取当前表中记录号为rid的记录
 * @param {Rid&} rid 记录号，指定记录的位置
 * @param {Context*} context
 * @return {unique_ptr<RmRecord>} rid对应的记录对象指针
 */
std::unique_ptr<RmRecord> RmFileHandle::get_record(const Rid& rid, Context* context) const {
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    std::unique_ptr<RmRecord> record;
    try {
        PageReadGuard page_guard(page_handle.page);
        // 检查目标槽位是否真的存储了记录
        if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
            throw RecordNotFoundError(rid.page_no, rid.slot_no);
        }
        // 深拷贝记录数据，使返回的 RmRecord 生命周期独立于 page
        record = std::make_unique<RmRecord>(file_hdr_.record_size, page_handle.get_slot(rid.slot_no));
    } catch (...) {
        buffer_pool_manager_->unpin_page(page_handle.page, false);
        throw;
    }
    buffer_pool_manager_->unpin_page(page_handle.page, false);
    return record;
}

Rid RmFileHandle::prepare_insert_rid() {
    std::lock_guard<std::mutex> heap_lock(heap_mutex_);
    int page_no = file_hdr_.first_free_page_no;
    while (page_no != RM_NO_PAGE) {
        RmPageHandle page_handle = fetch_page_handle(page_no);
        int slot_no;
        int next_page_no;
        {
            PageReadGuard page_guard(page_handle.page);
            slot_no = first_available_slot(page_handle);
            next_page_no = page_handle.page_hdr->next_free_page_no;
        }
        buffer_pool_manager_->unpin_page(page_handle.page, false);
        if (slot_no < file_hdr_.num_records_per_page) {
            return Rid{page_no, slot_no};
        }
        page_no = next_page_no;
    }
    return Rid{file_hdr_.num_pages, 0};
}

void RmFileHandle::add_active_insert_page_unlocked(int page_no) {
    if (std::find(active_insert_pages_.begin(), active_insert_pages_.end(), page_no) == active_insert_pages_.end()) {
        active_insert_pages_.push_back(page_no);
    }
}

void RmFileHandle::remove_active_insert_page_unlocked(int page_no) {
    active_insert_pages_.erase(std::remove(active_insert_pages_.begin(), active_insert_pages_.end(), page_no),
                               active_insert_pages_.end());
    if (active_insert_pages_.empty()) {
        active_insert_cursor_ = 0;
    } else {
        active_insert_cursor_ %= active_insert_pages_.size();
    }
}

void RmFileHandle::ensure_active_insert_pages_unlocked(size_t desired_pages) {
    desired_pages = std::max<size_t>(1, std::min(desired_pages, kMaxActiveInsertPages));

    int page_no = file_hdr_.first_free_page_no;
    std::unordered_set<int> visited;
    while (page_no != RM_NO_PAGE && active_insert_pages_.size() < desired_pages && visited.insert(page_no).second) {
        RmPageHandle page_handle = fetch_page_handle(page_no);
        int next_page_no = RM_NO_PAGE;
        bool has_slot = false;
        {
            PageReadGuard page_guard(page_handle.page);
            next_page_no = page_handle.page_hdr->next_free_page_no;
            has_slot = page_has_available_slot(page_handle);
        }
        buffer_pool_manager_->unpin_page(page_handle.page, false);
        if (has_slot) {
            add_active_insert_page_unlocked(page_no);
        }
        page_no = next_page_no;
    }

    while (active_insert_pages_.size() < desired_pages) {
        RmPageHandle page_handle = create_new_page_handle_unlocked();
        int new_page_no = page_handle.page->get_page_id().page_no;
        add_active_insert_page_unlocked(new_page_no);
        buffer_pool_manager_->unpin_page(page_handle.page, true);
    }
}

RmFileHandle::InsertSlotReservation RmFileHandle::reserve_insert_slot() {
    std::lock_guard<std::mutex> heap_lock(heap_mutex_);

    // 固定维护有界活跃页集合，使同表写者无需先在一个页上形成争用后才被动扩容。
    // 每表最多增加 15 个半满页（约 60KB），空间上界固定且不依赖表名或数据分布。
    ensure_active_insert_pages_unlocked(kMaxActiveInsertPages);

    for (;;) {
        size_t page_count = active_insert_pages_.size();
        for (size_t attempt = 0; attempt < page_count; ++attempt) {
            size_t index = (active_insert_cursor_ + attempt) % page_count;
            int candidate_page_no = active_insert_pages_[index];
            RmPageHandle page_handle = fetch_page_handle(candidate_page_no);
            int slot_no;
            {
                PageReadGuard page_guard(page_handle.page);
                slot_no = first_available_slot(page_handle);
            }
            buffer_pool_manager_->unpin_page(page_handle.page, false);
            if (slot_no >= file_hdr_.num_records_per_page) {
                continue;
            }

            reserved_slots_[candidate_page_no].insert(slot_no);
            ++reserved_slot_count_;
            active_insert_cursor_ = (index + 1) % page_count;
            return InsertSlotReservation(this, Rid{candidate_page_no, slot_no});
        }

        // 所有活跃页的可用槽都已被其它线程预约，立即增加一个活跃页而不是等待同一页。
        RmPageHandle page_handle = create_new_page_handle_unlocked();
        int new_page_no = page_handle.page->get_page_id().page_no;
        add_active_insert_page_unlocked(new_page_no);
        buffer_pool_manager_->unpin_page(page_handle.page, true);
    }
}

/**
 * @description: 在当前表中插入一条记录，不指定插入位置
 * @param {char*} buf 要插入的记录的数据
 * @param {Context*} context
 * @return {Rid} 插入的记录的记录号（位置）
 */
Rid RmFileHandle::insert_record(char* buf, Context* context, const std::string &table_name) {
    std::lock_guard<std::mutex> heap_lock(heap_mutex_);
    // 获取一个有空闲槽位的页（优先复用 first_free_page_no 指向的页，避免频繁分配新页）
    RmPageHandle page_handle = create_page_handle();
    PageId page_id = page_handle.page->get_page_id();
    Rid rid;
    bool dirty = false;
    try {
        PageWriteGuard page_guard(page_handle.page);
        // 在 bitmap 中找到第一个为 0 的位，即空闲槽位
        int slot_no = first_available_slot(page_handle);
        assert(slot_no < file_hdr_.num_records_per_page);

        rid = Rid{page_id.page_no, slot_no};
        RmRecord new_record(file_hdr_.record_size, buf);
        lsn_t page_lsn = INVALID_LSN;
        if (context != nullptr && context->txn_ != nullptr && context->log_mgr_ != nullptr && !table_name.empty()) {
            InsertLogRecord log_record(context->txn_->get_transaction_id(), new_record, rid, table_name);
            page_lsn = append_heap_log(log_record, context);
        }

        memcpy(page_handle.get_slot(slot_no), buf, file_hdr_.record_size);
        Bitmap::set(page_handle.bitmap, slot_no);
        page_handle.page_hdr->num_records++;
        dirty = true;
        MarkPageWalState(page_handle.page, page_lsn);

        // 插入后页面已满，将其从空闲链表中摘除
        if (!page_has_available_slot(page_handle)) {
            unlink_free_page_if_needed(page_handle, page_lsn);
        }
    } catch (...) {
        buffer_pool_manager_->unpin_page(page_handle.page, dirty);
        throw;
    }

    buffer_pool_manager_->unpin_page(page_handle.page, true);
    return rid;
}

/**
 * @description: 在当前表中的指定位置插入一条记录
 * @param {Rid&} rid 要插入记录的位置
 * @param {char*} buf 要插入记录的数据
 */
void RmFileHandle::insert_record(const Rid& rid, char* buf, lsn_t page_lsn) {
    std::lock_guard<std::mutex> heap_lock(heap_mutex_);
    // 主要用于事务回滚时重新插入已删除的记录
    // 循环创建直到目标页号存在，处理跨多页的情况
    while (rid.page_no >= file_hdr_.num_pages) {
        RmPageHandle new_handle = create_new_page_handle_unlocked();
        {
            PageWriteGuard page_guard(new_handle.page);
            MarkPageWalState(new_handle.page, page_lsn);
        }
        buffer_pool_manager_->unpin_page(new_handle.page, true);
    }
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    bool dirty = false;
    try {
        PageWriteGuard page_guard(page_handle.page);
        // 目标 slot 已有记录则直接报错，防止 num_records 被错误累加
        if (Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
            throw InternalError("Slot already occupied: (" + std::to_string(rid.page_no) + "," + std::to_string(rid.slot_no) + ")");
        }

        memcpy(page_handle.get_slot(rid.slot_no), buf, file_hdr_.record_size);
        Bitmap::set(page_handle.bitmap, rid.slot_no);
        page_handle.page_hdr->num_records++;
        quarantined_slots_[rid.page_no].erase(rid.slot_no);
        dirty = true;
        MarkPageWalState(page_handle.page, page_lsn);

        // 插入后页面变满，需将其从空闲链表中摘除
        if (!page_has_available_slot(page_handle)) {
            unlink_free_page_if_needed(page_handle, page_lsn);
        }
    } catch (...) {
        buffer_pool_manager_->unpin_page(page_handle.page, dirty);
        throw;
    }

    buffer_pool_manager_->unpin_page(page_handle.page, true);
}

lsn_t RmFileHandle::insert_record(const Rid &rid, char *buf, Context *context, const std::string &table_name) {
    RmRecord new_record(file_hdr_.record_size, buf);
    InsertLogRecord log_record(context->txn_->get_transaction_id(), new_record, rid, table_name);
    lsn_t page_lsn = append_heap_log(log_record, context);
    insert_record(rid, buf, page_lsn);
    return page_lsn;
}

lsn_t RmFileHandle::insert_record(InsertSlotReservation &reservation, char *buf, Context *context,
                                  const std::string &table_name) {
    if (!reservation.valid() || reservation.owner_ != this) {
        throw InternalError("Invalid heap insert slot reservation");
    }

    const Rid rid = reservation.rid_;
    RmRecord new_record(file_hdr_.record_size, buf);
    InsertLogRecord log_record(context->txn_->get_transaction_id(), new_record, rid, table_name);
    // WAL 先于 bitmap/slot 发布；日志追加不再位于表级 INSERT 临界区内。
    lsn_t page_lsn = append_heap_log(log_record, context);

    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    bool dirty = false;
    bool page_became_full = false;
    try {
        PageWriteGuard page_guard(page_handle.page);
        if (Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
            throw InternalError("Reserved heap slot already occupied");
        }
        memcpy(page_handle.get_slot(rid.slot_no), buf, file_hdr_.record_size);
        Bitmap::set(page_handle.bitmap, rid.slot_no);
        ++page_handle.page_hdr->num_records;
        page_handle.page->set_page_lsn(page_lsn);
        page_became_full = page_handle.page_hdr->num_records == file_hdr_.num_records_per_page;
        dirty = true;
    } catch (...) {
        buffer_pool_manager_->unpin_page(page_handle.page, dirty);
        throw;
    }
    buffer_pool_manager_->unpin_page(page_handle.page, true);

    // 必须先释放页写锁再进入 heap 元数据锁，统一保持 heap_mutex_ -> page latch 的锁序。
    complete_insert_reservation(&reservation, page_became_full, page_lsn);
    return page_lsn;
}

/**
 * @description: 删除记录文件中记录号为rid的记录
 * @param {Rid&} rid 要删除的记录的记录号（位置）
 * @param {Context*} context
 */
void RmFileHandle::delete_record(const Rid& rid, Context* context, const std::string &table_name) {
    std::lock_guard<std::mutex> heap_lock(heap_mutex_);
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    bool dirty = false;
    try {
        PageWriteGuard page_guard(page_handle.page);
        // 检查目标槽位是否真的存在记录，防止重复删除导致 num_records 错误递减
        if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
            throw RecordNotFoundError(rid.page_no, rid.slot_no);
        }

        bool was_full = (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page);
        lsn_t page_lsn = INVALID_LSN;
        if (context != nullptr && context->txn_ != nullptr && context->log_mgr_ != nullptr && !table_name.empty()) {
            RmRecord old_record(file_hdr_.record_size, page_handle.get_slot(rid.slot_no));
            DeleteLogRecord log_record(context->txn_->get_transaction_id(), old_record, rid, table_name);
            page_lsn = append_heap_log(log_record, context);
        }
        Bitmap::reset(page_handle.bitmap, rid.slot_no);
        page_handle.page_hdr->num_records--;
        dirty = true;
        MarkPageWalState(page_handle.page, page_lsn);

        // 页面从全满变为有空闲时，将其重新加入空闲链表，以便后续插入复用
        if (was_full) {
            release_page_handle(page_handle);
        }
    } catch (...) {
        buffer_pool_manager_->unpin_page(page_handle.page, dirty);
        throw;
    }
    buffer_pool_manager_->unpin_page(page_handle.page, true);
}

void RmFileHandle::delete_record_quarantine(const Rid &rid, lsn_t page_lsn) {
    std::lock_guard<std::mutex> heap_lock(heap_mutex_);
    // WAL-before-heap loser INSERT undo can see a RID whose page was never created
    // or whose file header was not persisted before crash.
    if (rid.page_no < 0 || rid.page_no >= file_hdr_.num_pages) {
        return;
    }
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    bool dirty = false;
    bool exists = true;
    {
        PageWriteGuard page_guard(page_handle.page);
        if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
            exists = false;
        } else {
            Bitmap::reset(page_handle.bitmap, rid.slot_no);
            page_handle.page_hdr->num_records--;
            quarantined_slots_[rid.page_no].insert(rid.slot_no);
            dirty = true;
            MarkPageWalState(page_handle.page, page_lsn);
        }
    }
    buffer_pool_manager_->unpin_page(page_handle.page, dirty);
    if (!exists) {
        return;
    }
}

lsn_t RmFileHandle::log_delete_record(const Rid &rid, const RmRecord &old_record, Context *context,
                                     const std::string &table_name) {
    if (context == nullptr || context->txn_ == nullptr || context->log_mgr_ == nullptr || table_name.empty()) {
        return INVALID_LSN;
    }
    {
        RmPageHandle page_handle = fetch_page_handle(rid.page_no);
        try {
            PageReadGuard page_guard(page_handle.page);
            if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
                throw RecordNotFoundError(rid.page_no, rid.slot_no);
            }
        } catch (...) {
            buffer_pool_manager_->unpin_page(page_handle.page, false);
            throw;
        }
        buffer_pool_manager_->unpin_page(page_handle.page, false);
    }
    DeleteLogRecord log_record(context->txn_->get_transaction_id(), old_record, rid, table_name);
    lsn_t page_lsn = append_heap_log(log_record, context);
    return page_lsn;
}

lsn_t RmFileHandle::log_update_record(const Rid &rid, const RmRecord &old_record, const RmRecord &new_record,
                                      Context *context, const std::string &table_name) {
    if (context == nullptr || context->txn_ == nullptr || context->log_mgr_ == nullptr || table_name.empty()) {
        return INVALID_LSN;
    }
    {
        RmPageHandle page_handle = fetch_page_handle(rid.page_no);
        try {
            PageReadGuard page_guard(page_handle.page);
            if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
                throw RecordNotFoundError(rid.page_no, rid.slot_no);
            }
        } catch (...) {
            buffer_pool_manager_->unpin_page(page_handle.page, false);
            throw;
        }
        buffer_pool_manager_->unpin_page(page_handle.page, false);
    }
    UpdateLogRecord log_record(context->txn_->get_transaction_id(), old_record, new_record, rid, table_name);
    lsn_t page_lsn = append_heap_log(log_record, context);
    return page_lsn;
}

/**
 * @description: 更新记录文件中记录号为rid的记录
 * @param {Rid&} rid 要更新的记录的记录号（位置）
 * @param {char*} buf 新记录的数据
 * @param {Context*} context
 */
void RmFileHandle::update_record(const Rid& rid, char* buf, Context* context, const std::string &table_name,
                                 const RmRecord *old_record, lsn_t page_lsn) {
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    bool dirty = false;
    try {
        PageWriteGuard page_guard(page_handle.page);
        // 检查目标槽位是否存在记录，防止覆盖无效位置
        if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
            throw RecordNotFoundError(rid.page_no, rid.slot_no);
        }
        if (page_lsn == INVALID_LSN && context != nullptr && context->txn_ != nullptr &&
            context->log_mgr_ != nullptr && !table_name.empty()) {
            RmRecord old_image = old_record == nullptr ? RmRecord(file_hdr_.record_size, page_handle.get_slot(rid.slot_no))
                                                       : RmRecord(*old_record);
            RmRecord new_image(file_hdr_.record_size, buf);
            UpdateLogRecord log_record(context->txn_->get_transaction_id(), old_image, new_image, rid, table_name);
            page_lsn = append_heap_log(log_record, context);
        }
        memcpy(page_handle.get_slot(rid.slot_no), buf, file_hdr_.record_size);
        dirty = true;
        MarkPageWalState(page_handle.page, page_lsn);
    } catch (...) {
        buffer_pool_manager_->unpin_page(page_handle.page, dirty);
        throw;
    }
    buffer_pool_manager_->unpin_page(page_handle.page, true);
}

/**
 * 以下函数为辅助函数，仅提供参考，可以选择完成如下函数，也可以删除如下函数，在单元测试中不涉及如下函数接口的直接调用
*/
/**
 * @description: 获取指定页面的页面句柄
 * @param {int} page_no 页面号
 * @return {RmPageHandle} 指定页面的句柄
 */
RmPageHandle RmFileHandle::fetch_page_handle(int page_no) const {
    if (page_no < 0 || page_no >= num_pages_snapshot()) {
        throw PageNotExistError("", page_no);
    }
    Page* page = buffer_pool_manager_->fetch_page({fd_, page_no});
    // buffer pool 全 pinned 时 fetch_page 返回 nullptr
    if (page == nullptr) {
        throw InternalError("Buffer pool exhausted, cannot fetch page " + std::to_string(page_no));
    }
    return RmPageHandle(&file_hdr_, page);
}

/**
 * @description: 创建一个新的page handle
 * @return {RmPageHandle} 新的PageHandle
 */
RmPageHandle RmFileHandle::create_new_page_handle() {
    std::lock_guard<std::mutex> heap_lock(heap_mutex_);
    return create_new_page_handle_unlocked();
}

RmPageHandle RmFileHandle::create_new_page_handle_unlocked() {
    PageId page_id{fd_, INVALID_PAGE_ID};
    Page* page = buffer_pool_manager_->new_page(&page_id);
    assert(page != nullptr);

    RmPageHandle page_handle(&file_hdr_, page);
    {
        PageWriteGuard page_guard(page);
        // 初始化新页的页头和 bitmap：记录数为 0，bitmap 全清零
        page_handle.page_hdr->num_records = 0;
        page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
        Bitmap::init(page_handle.bitmap, file_hdr_.bitmap_size);

        // 将新页插入空闲链表头部，并在初始化完成后发布单调 num_pages 快照。
        page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
        file_hdr_.first_free_page_no = page_id.page_no;
        file_hdr_.num_pages++;
        publish_num_pages_snapshot_unlocked();
        // 活跃插入页可能在事务生成首条 WAL 前预分配并单独写回；这一
        // 页头初始化明确属于无 WAL 的文件空间管理，不依赖 new_page 暗示。
        page->mark_wal_free_dirty();
    }
    return page_handle;
}

/**
 * @brief 创建或获取一个空闲的page handle
 *
 * @return RmPageHandle 返回生成的空闲page handle
 * @note pin the page, remember to unpin it outside!
 */
RmPageHandle RmFileHandle::create_page_handle() {
    if (file_hdr_.first_free_page_no == RM_NO_PAGE) {
        // 没有空闲页，分配新页
        return create_new_page_handle_unlocked();
    }
    while (file_hdr_.first_free_page_no != RM_NO_PAGE) {
        RmPageHandle page_handle = fetch_page_handle(file_hdr_.first_free_page_no);
        bool has_slot = false;
        {
            PageReadGuard page_guard(page_handle.page);
            has_slot = page_has_available_slot(page_handle);
        }
        if (has_slot) {
            return page_handle;
        }
        {
            PageWriteGuard page_guard(page_handle.page);
            if (!page_has_available_slot(page_handle)) {
                page_handle.page->mark_wal_free_dirty();
                unlink_free_page_if_needed(page_handle, INVALID_LSN);
            }
        }
        buffer_pool_manager_->unpin_page(page_handle.page, true);
    }
    return create_new_page_handle_unlocked();
}

/**
 * @description: 当一个页面从没有空闲空间的状态变为有空闲空间状态时，更新文件头和页头中空闲页面相关的元数据
 */
void RmFileHandle::release_page_handle(RmPageHandle& page_handle) {
    // 将此页重新插入空闲链表头部，使后续插入能够优先复用已有页面
    page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;
    file_hdr_.first_free_page_no = page_handle.page->get_page_id().page_no;
}

lsn_t RmFileHandle::append_heap_log(LogRecord &record, Context *context) {
    record.prev_lsn_ = context->txn_->get_prev_lsn();
    lsn_t lsn = context->log_mgr_->add_log_to_buffer(&record);
    context->txn_->set_prev_lsn(lsn);
    return lsn;
}

int RmFileHandle::first_available_slot(const RmPageHandle &page_handle) const {
    auto quarantine_it = quarantined_slots_.find(page_handle.page->get_page_id().page_no);
    auto reserved_it = reserved_slots_.find(page_handle.page->get_page_id().page_no);
    for (int slot_no = 0; slot_no < file_hdr_.num_records_per_page; ++slot_no) {
        if (Bitmap::is_set(page_handle.bitmap, slot_no)) {
            continue;
        }
        if (quarantine_it != quarantined_slots_.end() && quarantine_it->second.count(slot_no) > 0) {
            continue;
        }
        if (reserved_it != reserved_slots_.end() && reserved_it->second.count(slot_no) > 0) {
            continue;
        }
        return slot_no;
    }
    return file_hdr_.num_records_per_page;
}

bool RmFileHandle::page_has_available_slot(const RmPageHandle &page_handle) const {
    return first_available_slot(page_handle) < file_hdr_.num_records_per_page;
}

void RmFileHandle::cancel_insert_reservation(InsertSlotReservation *reservation) noexcept {
    if (reservation == nullptr || reservation->owner_ != this || !reservation->active_) {
        return;
    }
    try {
        std::lock_guard<std::mutex> heap_lock(heap_mutex_);
        auto page_it = reserved_slots_.find(reservation->rid_.page_no);
        if (page_it != reserved_slots_.end()) {
            size_t erased = page_it->second.erase(reservation->rid_.slot_no);
            if (erased > 0 && reserved_slot_count_ > 0) {
                --reserved_slot_count_;
            }
            if (page_it->second.empty()) {
                reserved_slots_.erase(page_it);
            }
        }
        add_active_insert_page_unlocked(reservation->rid_.page_no);
    } catch (...) {
        // noexcept 析构路径不能传播异常；预约只影响未发布槽位，失败至多损失一次复用机会。
    }
    reservation->active_ = false;
    reservation->owner_ = nullptr;
}

void RmFileHandle::complete_insert_reservation(InsertSlotReservation *reservation, bool page_became_full,
                                               lsn_t page_lsn) {
    if (reservation == nullptr || reservation->owner_ != this || !reservation->active_) {
        throw InternalError("Completing invalid heap insert reservation");
    }

    std::lock_guard<std::mutex> heap_lock(heap_mutex_);
    auto page_it = reserved_slots_.find(reservation->rid_.page_no);
    if (page_it == reserved_slots_.end() || page_it->second.erase(reservation->rid_.slot_no) == 0) {
        throw InternalError("Heap insert reservation was lost before publication");
    }
    if (reserved_slot_count_ > 0) {
        --reserved_slot_count_;
    }
    if (page_it->second.empty()) {
        reserved_slots_.erase(page_it);
    }

    if (page_became_full) {
        // 只有每页最后一个槽位发布时才触碰空闲链，普通 INSERT 的收尾只持极短的预约元数据锁。
        RmPageHandle page_handle = fetch_page_handle(reservation->rid_.page_no);
        {
            PageWriteGuard page_guard(page_handle.page);
            MarkPageWalState(page_handle.page, page_lsn);
            unlink_free_page_if_needed(page_handle, page_lsn);
        }
        buffer_pool_manager_->unpin_page(page_handle.page, true);
        remove_active_insert_page_unlocked(reservation->rid_.page_no);
    }

    reservation->active_ = false;
    reservation->owner_ = nullptr;
}

void RmFileHandle::unlink_free_page_if_needed(RmPageHandle &page_handle, lsn_t page_lsn) {
    int target = page_handle.page->get_page_id().page_no;
    if (file_hdr_.first_free_page_no == target) {
        file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
        page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
        return;
    }

    int prev_no = file_hdr_.first_free_page_no;
    while (prev_no != RM_NO_PAGE) {
        RmPageHandle prev_handle = fetch_page_handle(prev_no);
        int next_no;
        bool dirty = false;
        {
            PageWriteGuard prev_guard(prev_handle.page);
            next_no = prev_handle.page_hdr->next_free_page_no;
            if (next_no == target) {
                prev_handle.page_hdr->next_free_page_no = page_handle.page_hdr->next_free_page_no;
                page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
                MarkPageWalState(prev_handle.page, page_lsn);
                dirty = true;
            }
        }
        if (next_no == target) {
            buffer_pool_manager_->unpin_page(prev_handle.page, dirty);
            break;
        }
        buffer_pool_manager_->unpin_page(prev_handle.page, false);
        prev_no = next_no;
    }
}
