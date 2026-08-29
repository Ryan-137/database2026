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

#include <atomic>
#include <cstdint>
#include <cstring>
#include <shared_mutex>
#include <string>

#include "common/config.h"

/**
 * @description: 存储层每个Page的id的声明
 */
struct PageId {
    int fd;  //  Page所在的磁盘文件开启后的文件描述符, 来定位打开的文件在内存中的位置
    page_id_t page_no = INVALID_PAGE_ID;

    friend bool operator==(const PageId &x, const PageId &y) { return x.fd == y.fd && x.page_no == y.page_no; }
    bool operator<(const PageId& x) const {
        if (fd != x.fd) return fd < x.fd;
        return page_no < x.page_no;
    }

    std::string toString() const {
        return "{fd: " + std::to_string(fd) + " page_no: " + std::to_string(page_no) + "}"; 
    }

    inline int64_t Get() const {
        return (static_cast<int64_t>(fd << 16) | page_no);
    }
};

// PageId的自定义哈希算法, 用于构建unordered_map<PageId, frame_id_t, PageIdHash>
struct PageIdHash {
    size_t operator()(const PageId &x) const { return (x.fd << 16) | x.page_no; }
};

template <>
struct std::hash<PageId> {
    size_t operator()(const PageId &obj) const { return std::hash<int64_t>()(obj.Get()); }
};

/**
 * @description: Page类声明, Page是RMDB数据块的单位、是负责数据操作Record模块的操作对象，
 * Page对象在磁盘上有文件存储, 若在Buffer中则有帧偏移, 并非特指Buffer或Disk上的数据
 */
class Page {
    friend class BufferPoolManager;

   public:
    
    Page() { reset_memory(); }

    ~Page() = default;

    PageId get_page_id() const { return id_; }

    inline char *get_data() { return data_; }

    bool is_dirty() const { return is_dirty_; }

    static constexpr size_t OFFSET_PAGE_START = 0;
    // 磁盘 heap 页历史布局在开头保留了 4 字节。继续保留该空洞，避免移动
    // RmPageHdr；运行期 page LSN 已迁移到 Page 帧的 sidecar，不能再与
    // IxPageHdr::next_free_page_no 共用 data[0..3]。
    static constexpr size_t OFFSET_LSN = 0;
    static constexpr size_t OFFSET_PAGE_HDR = 4;

    enum class WalDirtyState : std::uint8_t {
        kClean = 0,
        kWalBacked,
        kWalFree,
        kUnknown,
    };

    lsn_t get_page_lsn() const { return page_lsn_.load(std::memory_order_acquire); }
    WalDirtyState get_wal_dirty_state() const {
        return wal_dirty_state_.load(std::memory_order_acquire);
    }

    // 并发事务可能按 LSN 逆序获得同一页写锁；页头只能发布已包含日志的最大 LSN，禁止回退。
    inline void set_page_lsn(lsn_t page_lsn) {
        if (page_lsn == INVALID_LSN) {
            return;
        }
        lsn_t current = page_lsn_.load(std::memory_order_relaxed);
        while ((current == INVALID_LSN || page_lsn > current) &&
               !page_lsn_.compare_exchange_weak(current, page_lsn, std::memory_order_release,
                                                std::memory_order_relaxed)) {
        }
        auto state = wal_dirty_state_.load(std::memory_order_acquire);
        // Unknown 表示已有一轮修改漏报来源，后续合法 WAL 不能洗掉它；
        // 仍更新最大 LSN，便于诊断，但 provenance 保持 fail-closed。
        while (state != WalDirtyState::kUnknown && state != WalDirtyState::kWalBacked &&
               !wal_dirty_state_.compare_exchange_weak(state, WalDirtyState::kWalBacked,
                                                       std::memory_order_release,
                                                       std::memory_order_acquire)) {
        }
    }

    // load、恢复重建以及已持久化提交后的索引维护没有新的 WAL 依赖，调用方
    // 必须显式声明，禁止把漏设 page_lsn 的普通 DML 静默当成无 WAL 页面。
    void mark_wal_free_dirty() {
        auto state = wal_dirty_state_.load(std::memory_order_acquire);
        // 只允许 clean -> WAL-free。已有 WAL-backed 依赖不能被降级；
        // unknown 也必须保持 fail-closed，后续声明不能掩盖更早的漏标。
        while (state == WalDirtyState::kClean &&
               !wal_dirty_state_.compare_exchange_weak(state, WalDirtyState::kWalFree,
                                                       std::memory_order_release,
                                                       std::memory_order_acquire)) {
        }
    }

    void RLatch() const {
        page_latch_.lock_shared();
    }

    void RUnlatch() const {
        page_latch_.unlock_shared();
    }

    void WLatch() {
        page_latch_.lock();
    }

    void WUnlatch() {
        page_latch_.unlock();
    }

   private:
    void reset_memory() {
        memset(data_, OFFSET_PAGE_START, PAGE_SIZE);
        page_lsn_.store(INVALID_LSN, std::memory_order_relaxed);
        wal_dirty_state_.store(WalDirtyState::kClean, std::memory_order_relaxed);
    }

    /** page的唯一标识符 */
    PageId id_;

    /** The actual data that is stored within a page.
     *  该页面在bufferPool中的偏移地址
     */
    char data_[PAGE_SIZE] = {};

    /** 脏页判断；以下两个 bool/小原子并排，避免百万帧池产生额外对齐空洞。 */
    bool is_dirty_ = false;
    std::atomic<WalDirtyState> wal_dirty_state_{WalDirtyState::kClean};
    // 以下字段均由所属 BufferPool 分区锁保护。
    bool is_evictable_ = false;

    // 运行期 WAL 元数据只属于当前 frame 中的脏版本。当前恢复始终重放 heap
    // WAL 并从 heap 重建索引，不依赖磁盘 pageLSN 做 redo-skip，因此 frame
    // 复用时清空 sidecar 既安全又避免修改既有磁盘格式。
    std::atomic<lsn_t> page_lsn_{INVALID_LSN};

    /** The pin count of this page. */
    int pin_count_ = 0;

    // resident pin 是文件句柄生命周期内保留的基础 pin。它阻止热点结构页被
    // 淘汰，但不代表前台正在访问；后台写回据此仍可在没有普通访问者时把页
    // 标记为 clean。该字段与 pin_count_ 一样由所属 BufferPool 分区锁保护。
    int resident_pin_count_ = 0;

    // 每次发布新的页内容时递增。后台写回用它判断快照之后是否
    // 又有并发修改，避免把新的脏版本误标为已落盘。该字段由所属
    // BufferPool 分区锁保护。
    std::uint64_t dirty_generation_ = 0;

    mutable std::shared_mutex page_latch_;
};

class PageReadGuard {
public:
    explicit PageReadGuard(Page *page) : page_(page) {
        if (page_ != nullptr) page_->RLatch();
    }
    ~PageReadGuard() {
        if (page_ != nullptr) page_->RUnlatch();
    }
    PageReadGuard(const PageReadGuard &) = delete;
    PageReadGuard &operator=(const PageReadGuard &) = delete;

private:
    Page *page_;
};

class PageWriteGuard {
public:
    explicit PageWriteGuard(Page *page) : page_(page) {
        if (page_ != nullptr) page_->WLatch();
    }
    ~PageWriteGuard() {
        if (page_ != nullptr) page_->WUnlatch();
    }
    PageWriteGuard(const PageWriteGuard &) = delete;
    PageWriteGuard &operator=(const PageWriteGuard &) = delete;

private:
    Page *page_;
};
