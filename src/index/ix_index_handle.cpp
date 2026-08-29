/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "ix_index_handle.h"

#include <cstdlib>
#include <cstring>

#include "ix_scan.h"

namespace {

bool IndexHeaderDirtyFlushEnabled() {
    static const bool enabled = [] {
        const char *value = std::getenv("RMDB_ENABLE_INDEX_HEADER_DIRTY_FLUSH");
        // 默认启用；显式设置为 0 时使用原有逐次刷新路径。
        return value == nullptr || std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

struct IxDynamicHeaderState {
    page_id_t first_free_page_no;
    int num_pages;
    page_id_t root_page;
    page_id_t first_leaf;
    page_id_t last_leaf;

    bool operator==(const IxDynamicHeaderState &other) const {
        return first_free_page_no == other.first_free_page_no && num_pages == other.num_pages &&
               root_page == other.root_page && first_leaf == other.first_leaf && last_leaf == other.last_leaf;
    }
};

IxDynamicHeaderState CaptureDynamicHeaderState(const IxFileHdr &file_hdr) {
    return {
        file_hdr.first_free_page_no_,
        file_hdr.num_pages_,
        file_hdr.root_page_,
        file_hdr.first_leaf_,
        file_hdr.last_leaf_,
    };
}

}  // namespace

/**
 * @brief 在当前node中查找第一个>=target的key_idx
 *
 * @return key_idx，范围为[0,num_key)，如果返回的key_idx=num_key，则表示target大于最后一个key
 * @note 返回key index（同时也是rid index），作为slot no
 */
int IxNodeHandle::lower_bound(const char *target) const {
    // 二分查找，返回第一个 >= target 的 key 下标，范围 [0, num_key]
    int lo = 0, hi = page_hdr->num_key;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (ix_compare(get_key(mid), target, file_hdr->col_types_, file_hdr->col_lens_) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/**
 * @brief 在当前node中查找第一个>target的key_idx
 *
 * @return key_idx，范围为[1,num_key)，如果返回的key_idx=num_key，则表示target大于等于最后一个key
 * @note 注意此处的范围从1开始
 */
int IxNodeHandle::upper_bound(const char *target) const {
    // 二分查找，返回第一个 > target 的 key 下标，范围 [0, num_key]
    int lo = 0, hi = page_hdr->num_key;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (ix_compare(get_key(mid), target, file_hdr->col_types_, file_hdr->col_lens_) <= 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/**
 * @brief 用于叶子结点根据key来查找该结点中的键值对
 * 值value作为传出参数，函数返回是否查找成功
 *
 * @param key 目标key
 * @param[out] value 传出参数，目标key对应的Rid
 * @return 目标key是否存在
 */
bool IxNodeHandle::leaf_lookup(const char *key, Rid **value) {
    int pos = lower_bound(key);
    // lower_bound 找到的是第一个 >= key 的位置，需验证是否精确相等
    if (pos < page_hdr->num_key &&
        ix_compare(get_key(pos), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        *value = get_rid(pos);
        return true;
    }
    return false;
}

/**
 * 用于内部结点（非叶子节点）查找目标key所在的孩子结点（子树）
 * @param key 目标key
 * @return page_id_t 目标key所在的孩子节点（子树）的存储页面编号
 */
page_id_t IxNodeHandle::internal_lookup(const char *key) {
    // 内部节点中 keys[i] 是第 i 个子树中最小键
    // upper_bound(key) 返回第一个 > key 的位置，向左退一格即为目标子树
    int pos = upper_bound(key);
    // 保底取第 0 个子树（当所有 key 都 > key 时，即 pos==0 的情形不应出现在正常查找中，
    // 但为健壮性起见，至少返回 rids[0].page_no）
    if (pos == 0) {
        pos = 1;
    }
    return get_rid(pos - 1)->page_no;
}

/**
 * @brief 在指定位置插入n个连续的键值对
 * 将key的前n位插入到原来keys中的pos位置；将rid的前n位插入到原来rids中的pos位置
 *
 * @param pos 要插入键值对的位置
 * @param (key, rid) 连续键值对的起始地址，也就是第一个键值对，可以通过(key, rid)来获取n个键值对
 * @param n 键值对数量
 * @note [0,pos)           [pos,num_key)
 *                            key_slot
 *                            /      \
 *                           /        \
 *       [0,pos)     [pos,pos+n)   [pos+n,num_key+n)
 *                      key           key_slot
 */
void IxNodeHandle::insert_pairs(int pos, const char *key, const Rid *rid, int n) {
    assert(pos >= 0 && pos <= page_hdr->num_key);
    int key_size = file_hdr->col_tot_len_;
    int num = page_hdr->num_key;

    // 将 [pos, num) 的 key 向右移 n 位，腾出插入空间
    if (pos < num) {
        memmove(keys + (pos + n) * key_size,
                keys + pos * key_size,
                (num - pos) * key_size);
        memmove(rids + pos + n,
                rids + pos,
                (num - pos) * sizeof(Rid));
    }
    // 写入新的 n 个键值对
    memcpy(keys + pos * key_size, key, n * key_size);
    memcpy(rids + pos, rid, n * sizeof(Rid));
    page_hdr->num_key += n;
}

/**
 * @brief 用于在结点中插入单个键值对。
 * 函数返回插入后的键值对数量
 *
 * @param (key, value) 要插入的键值对
 * @return int 键值对数量
 */
int IxNodeHandle::insert(const char *key, const Rid &value) {
    int pos = lower_bound(key);
    // 唯一索引：key 已存在时拒绝插入
    if (pos < page_hdr->num_key &&
        ix_compare(get_key(pos), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        throw DuplicateKeyError();
    }
    insert_pair(pos, key, value);
    return page_hdr->num_key;
}

/**
 * @brief 用于在结点中的指定位置删除单个键值对
 *
 * @param pos 要删除键值对的位置
 */
void IxNodeHandle::erase_pair(int pos) {
    assert(pos >= 0 && pos < page_hdr->num_key);
    int key_size = file_hdr->col_tot_len_;
    int num = page_hdr->num_key;

    // 将 [pos+1, num) 的键值对整体向左移一位，覆盖 pos 处数据
    if (pos < num - 1) {
        memmove(keys + pos * key_size,
                keys + (pos + 1) * key_size,
                (num - pos - 1) * key_size);
        memmove(rids + pos,
                rids + pos + 1,
                (num - pos - 1) * sizeof(Rid));
    }
    page_hdr->num_key--;
}

/**
 * @brief 用于在结点中删除指定key的键值对。函数返回删除后的键值对数量
 *
 * @param key 要删除的键值对key值
 * @return 完成删除操作后的键值对数量
 */
int IxNodeHandle::remove(const char *key) {
    int pos = lower_bound(key);
    if (pos < page_hdr->num_key &&
        ix_compare(get_key(pos), key, file_hdr->col_types_, file_hdr->col_lens_) == 0) {
        erase_pair(pos);
    }
    return page_hdr->num_key;
}

IxIndexHandle::IxIndexHandle(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, int fd)
    : disk_manager_(disk_manager), buffer_pool_manager_(buffer_pool_manager), fd_(fd) {
    // 从磁盘第 0 页反序列化文件头，获取 B+ 树的元信息
    char* buf = new char[PAGE_SIZE];
    memset(buf, 0, PAGE_SIZE);
    disk_manager_->read_page(fd, IX_FILE_HDR_PAGE, buf, PAGE_SIZE);
    file_hdr_ = new IxFileHdr();
    file_hdr_->deserialize(buf);
    delete[] buf;  // 修复原始代码的内存泄漏

    // 修复原始 Bug：原代码用 now_page_no+1（always 1），正确做法是用实际页数初始化分配计数器
    // 保证后续 allocate_page() 从 num_pages_ 之后开始分配，不覆盖已有页
    disk_manager_->set_fd2pageno(fd, file_hdr_->num_pages_);
}

IxIndexHandle::~IxIndexHandle() {
    release_resident_root();
    delete file_hdr_;
}

Page *IxIndexHandle::get_resident_internal_root_locked() const {
    Page *published = resident_root_page_.load(std::memory_order_acquire);
    if (published != nullptr &&
        resident_root_page_no_.load(std::memory_order_relaxed) == file_hdr_->root_page_) {
        return published;
    }
    if (known_leaf_root_page_no_.load(std::memory_order_acquire) == file_hdr_->root_page_) {
        return nullptr;
    }

    std::scoped_lock resident_lock{resident_root_latch_};
    published = resident_root_page_.load(std::memory_order_relaxed);
    if (published != nullptr &&
        resident_root_page_no_.load(std::memory_order_relaxed) == file_hdr_->root_page_) {
        return published;
    }
    if (known_leaf_root_page_no_.load(std::memory_order_relaxed) == file_hdr_->root_page_) {
        return nullptr;
    }
    if (file_hdr_->root_page_ == IX_NO_PAGE) {
        return nullptr;
    }

    Page *candidate = buffer_pool_manager_->fetch_resident_page({fd_, file_hdr_->root_page_});
    if (candidate == nullptr) {
        return nullptr;
    }
    bool is_internal = false;
    {
        PageReadGuard root_guard(candidate);
        IxNodeHandle root(file_hdr_, candidate);
        is_internal = !root.is_leaf_page();
    }
    if (!is_internal) {
        buffer_pool_manager_->release_resident_page(candidate, false);
        known_leaf_root_page_no_.store(file_hdr_->root_page_, std::memory_order_release);
        return nullptr;
    }

    known_leaf_root_page_no_.store(IX_NO_PAGE, std::memory_order_relaxed);
    resident_root_page_no_.store(file_hdr_->root_page_, std::memory_order_relaxed);
    resident_root_page_.store(candidate, std::memory_order_release);
    return candidate;
}

void IxIndexHandle::release_resident_root() const {
    std::scoped_lock resident_lock{resident_root_latch_};
    Page *resident = resident_root_page_.exchange(nullptr, std::memory_order_acq_rel);
    if (resident != nullptr) {
        [[maybe_unused]] const bool released =
            buffer_pool_manager_->release_resident_page(resident, false);
        assert(released);
    }
    resident_root_page_no_.store(IX_NO_PAGE, std::memory_order_relaxed);
    known_leaf_root_page_no_.store(IX_NO_PAGE, std::memory_order_release);
}

void IxIndexHandle::update_root_page_no(page_id_t root) {
    if (file_hdr_->root_page_ == root) {
        return;
    }
    // 调用者持有 root_latch_ 独占锁，先撤销旧根的 resident pin，下一次
    // 遍历再按新页类型惰性建立缓存。
    release_resident_root();
    file_hdr_->root_page_ = root;
}

bool IxIndexHandle::InsertLeafTicketEnabled() {
    // 开关在首次访问后固定，便于使用同一二进制做进程级 A/B。
    static const bool enabled = [] {
        const char *value = std::getenv("RMDB_ENABLE_INDEX_INSERT_LEAF_TICKET");
        return value == nullptr || std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

bool IxIndexHandle::ConcurrentLeafInsertEnabled() {
    static const bool enabled = [] {
        const char *value = std::getenv("RMDB_ENABLE_CONCURRENT_INDEX_LEAF_INSERT");
        return value == nullptr || std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

/**
 * @brief 用于查找指定键所在的叶子结点
 * @param key 要查找的目标key值
 * @param operation 查找到目标键值对后要进行的操作类型
 * @param transaction 事务参数，如果不需要则默认传入nullptr
 * @return [leaf node] and [root_is_latched] 返回目标叶子结点以及根结点是否加锁
 * @note need to Unlatch and unpin the leaf node outside!
 * 注意：用了FindLeafPage之后一定要unlatch叶结点，否则下次latch该结点会堵塞！
 */
std::pair<IxNodeHandle *, bool> IxIndexHandle::find_leaf_page(const char *key, Operation operation,
                                                            Transaction *transaction, bool find_first) const {
    Page *resident_root = get_resident_internal_root_locked();
    bool borrowed_resident_root = resident_root != nullptr;
    IxNodeHandle *node = borrowed_resident_root ? new IxNodeHandle(file_hdr_, resident_root)
                                                : fetch_node(file_hdr_->root_page_);
    // 从根节点向下遍历，沿路 unpin 已访问的内部节点
    while (!node->is_leaf_page()) {
        page_id_t child_page;
        if (find_first) {
            // find_first 模式：始终走最左子树，用于范围扫描起点定位
            child_page = node->value_at(0);
        } else {
            child_page = node->internal_lookup(key);
        }
        if (!borrowed_resident_root) {
            buffer_pool_manager_->unpin_page(node->page, false);
        }
        delete node;
        node = fetch_node(child_page);
        borrowed_resident_root = false;
    }
    return {node, false};
}

/**
 * @brief 用于查找指定键在叶子结点中的对应的值result
 *
 * @param key 查找的目标key值
 * @param result 用于存放结果的容器
 * @param transaction 事务指针
 * @return bool 返回目标键值对是否存在
 */
bool IxIndexHandle::get_value(const char *key, Rid *result, Transaction *transaction,
                              IxInsertLeafTicket *insert_ticket) {
    if (insert_ticket != nullptr) {
        *insert_ticket = IxInsertLeafTicket{};
    }
    std::shared_lock<std::shared_mutex> lock{root_latch_};

    // 长键可能需要分配内存，必须在 fetch 叶页之前完成，避免异常路径泄漏 pin。
    if (insert_ticket != nullptr) {
        insert_ticket->owner_ = this;
        insert_ticket->key_len_ = static_cast<std::uint16_t>(file_hdr_->col_tot_len_);
        if (file_hdr_->col_tot_len_ <= static_cast<int>(IxInsertLeafTicket::kInlineKeyBytes)) {
            memcpy(insert_ticket->inline_key_.data(), key, file_hdr_->col_tot_len_);
        } else {
            insert_ticket->overflow_key_.assign(key, key + file_hdr_->col_tot_len_);
        }
    }

    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::FIND, transaction);
    if (leaf == nullptr) {
        return false;
    }
    bool found = false;
    {
        PageReadGuard leaf_guard(leaf->page);
        if (insert_ticket != nullptr) {
            insert_ticket->leaf_page_no_ = leaf->get_page_no();
            insert_ticket->routing_epoch_ = structural_epoch_;
            insert_ticket->valid_ = true;
        }
        Rid *rid_ptr = nullptr;
        found = leaf->leaf_lookup(key, &rid_ptr);
        if (found) {
            *result = *rid_ptr;
        }
    }
    buffer_pool_manager_->unpin_page(leaf->page, false);
    delete leaf;
    return found;
}

bool IxIndexHandle::get_value(const char *key, std::vector<Rid> *result, Transaction *transaction,
                              IxInsertLeafTicket *insert_ticket) {
    Rid rid;
    if (!get_value(key, &rid, transaction, insert_ticket)) {
        return false;
    }
    result->push_back(rid);
    return true;
}

/**
 * @brief  将传入的一个node拆分(Split)成两个结点，在node的右边生成一个新结点new node
 * @param node 需要拆分的结点
 * @return 拆分得到的new_node
 * @note need to unpin the new node outside
 * 注意：本函数执行完毕后，原node和new node都需要在函数外面进行unpin
 */
IxNodeHandle *IxIndexHandle::split(IxNodeHandle *node, lsn_t page_lsn) {
    BumpRoutingEpochLocked();
    IxNodeHandle *new_node = create_node();
    // 新节点继承相同的 is_leaf 属性和父节点引用
    new_node->page_hdr->is_leaf = node->page_hdr->is_leaf;
    new_node->page_hdr->parent = node->page_hdr->parent;
    new_node->page_hdr->num_key = 0;

    // 右半部分（从 split_idx 开始）移到新节点
    int split_idx = node->get_size() / 2;
    int new_key_num = node->get_size() - split_idx;
    new_node->insert_pairs(0, node->get_key(split_idx), node->get_rid(split_idx), new_key_num);
    node->set_size(split_idx);

    if (node->is_leaf_page()) {
        // 叶节点：维护双向链表，new_node 插入 node 和 node->next 之间
        new_node->set_next_leaf(node->get_next_leaf());
        new_node->set_prev_leaf(node->get_page_no());

        // 更新 node 原来的后继节点的 prev 指针
        if (node->get_next_leaf() != IX_LEAF_HEADER_PAGE) {
            IxNodeHandle *next = fetch_node(node->get_next_leaf());
            next->set_prev_leaf(new_node->get_page_no());
            mark_node_dirty(next, page_lsn);
            buffer_pool_manager_->unpin_page(next->page, true);
            delete next;
        } else {
            // node 原来是最后一个叶节点，分裂后 new_node 成为最后一个叶节点
            // 同时更新 LEAF_HEADER 的 prev 指针（ix_scan 依赖）
            IxNodeHandle *header = fetch_node(IX_LEAF_HEADER_PAGE);
            header->set_prev_leaf(new_node->get_page_no());
            mark_node_dirty(header, page_lsn);
            buffer_pool_manager_->unpin_page(header->page, true);
            delete header;
        }
        node->set_next_leaf(new_node->get_page_no());

        // 若原节点是 last_leaf，分裂后 new_node 成为新的 last_leaf
        if (file_hdr_->last_leaf_ == node->get_page_no()) {
            file_hdr_->last_leaf_ = new_node->get_page_no();
        }
    } else {
        // 内部节点：更新 new_node 所有孩子的父指针
        for (int i = 0; i < new_node->get_size(); i++) {
            maintain_child(new_node, i, page_lsn);
        }
    }
    return new_node;
}

/**
 * @brief Insert key & value pair into internal page after split
 * 拆分(Split)后，向上找到old_node的父结点
 * 将new_node的第一个key插入到父结点，其位置在 父结点指向old_node的孩子指针 之后
 * 如果插入后>=maxsize，则必须继续拆分父结点，然后在其父结点的父结点再插入，即需要递归
 * 直到找到的old_node为根结点时，结束递归（此时将会新建一个根R，关键字为key，old_node和new_node为其孩子）
 *
 * @param (old_node, new_node) 原结点为old_node，old_node被分裂之后产生了新的右兄弟结点new_node
 * @param key 要插入parent的key
 * @note 一个结点插入了键值对之后需要分裂，分裂后左半部分的键值对保留在原结点，在参数中称为old_node，
 * 右半部分的键值对分裂为新的右兄弟节点，在参数中称为new_node（参考Split函数来理解old_node和new_node）
 * @note 本函数执行完毕后，new node和old node都需要在函数外面进行unpin
 */
void IxIndexHandle::insert_into_parent(IxNodeHandle *old_node, const char *key, IxNodeHandle *new_node,
                                       Transaction *transaction, lsn_t page_lsn) {
    BumpRoutingEpochLocked();
    if (old_node->is_root_page()) {
        // 分裂的是根节点，需要新建根，树高度增加 1
        IxNodeHandle *new_root = create_node();
        new_root->page_hdr->is_leaf = false;
        new_root->page_hdr->parent = IX_NO_PAGE;
        new_root->page_hdr->num_key = 0;

        // 新根的两个子节点：left=old_node，right=new_node
        Rid left_rid{old_node->get_page_no(), 0};
        Rid right_rid{new_node->get_page_no(), 0};
        new_root->insert_pair(0, old_node->get_key(0), left_rid);
        new_root->insert_pair(1, key, right_rid);

        old_node->set_parent_page_no(new_root->get_page_no());
        new_node->set_parent_page_no(new_root->get_page_no());
        update_root_page_no(new_root->get_page_no());

        mark_node_dirty(new_root, page_lsn);
        buffer_pool_manager_->unpin_page(new_root->page, true);
        delete new_root;
        return;
    }

    // 获取父节点，将 new_node 的第一个 key 以及对应的 rid 插入到 old_node 的位置之后
    IxNodeHandle *parent = fetch_node(old_node->get_parent_page_no());
    int idx = parent->find_child(old_node);
    Rid new_rid{new_node->get_page_no(), 0};
    parent->insert_pair(idx + 1, key, new_rid);
    new_node->set_parent_page_no(parent->get_page_no());

    // 父节点溢出则继续分裂，向上递归
    if (parent->get_size() >= parent->get_max_size()) {
        IxNodeHandle *new_parent = split(parent, page_lsn);
        insert_into_parent(parent, new_parent->get_key(0), new_parent, transaction, page_lsn);
        mark_node_dirty(new_parent, page_lsn);
        buffer_pool_manager_->unpin_page(new_parent->page, true);
        delete new_parent;
    }

    mark_node_dirty(parent, page_lsn);
    buffer_pool_manager_->unpin_page(parent->page, true);
    delete parent;
}

/**
 * @brief 将指定键值对插入到B+树中
 * @param (key, value) 要插入的键值对
 * @param transaction 事务指针
 * @return page_id_t 插入到的叶结点的page_no
 */
page_id_t IxIndexHandle::insert_entry(const char *key, const Rid &value, Transaction *transaction, lsn_t page_lsn,
                                      bool flush_header, IxInsertLeafTicket *insert_ticket) {
    if (ConcurrentLeafInsertEnabled()) {
        std::shared_lock<std::shared_mutex> shared_tree_lock{root_latch_};
        IxNodeHandle *fast_leaf = nullptr;
        const char *ticket_key = nullptr;
        if (insert_ticket != nullptr && insert_ticket->valid_) {
            ticket_key = insert_ticket->key_len_ <= IxInsertLeafTicket::kInlineKeyBytes
                             ? insert_ticket->inline_key_.data()
                             : insert_ticket->overflow_key_.data();
        }
        const bool ticket_matches = insert_ticket != nullptr && insert_ticket->valid_ &&
                                    insert_ticket->owner_ == this &&
                                    insert_ticket->routing_epoch_ == structural_epoch_ &&
                                    insert_ticket->key_len_ == file_hdr_->col_tot_len_ &&
                                    memcmp(ticket_key, key, file_hdr_->col_tot_len_) == 0 &&
                                    insert_ticket->leaf_page_no_ >= IX_INIT_ROOT_PAGE &&
                                    insert_ticket->leaf_page_no_ < file_hdr_->num_pages_;
        if (insert_ticket != nullptr) {
            // 快路径和独占回退都只允许消费一次；失效时重新从根定位。
            insert_ticket->valid_ = false;
        }
        if (ticket_matches) {
            fast_leaf = fetch_node(insert_ticket->leaf_page_no_);
        } else {
            fast_leaf = find_leaf_page(key, Operation::INSERT, transaction).first;
        }

        bool inserted = false;
        page_id_t fast_leaf_page_no = INVALID_PAGE_ID;
        try {
            PageWriteGuard leaf_guard(fast_leaf->page);
            // 插入后仍低于 split 阈值时只改一个叶页；结构变化统一退回 root 独占协议。
            if (fast_leaf->is_leaf_page() && fast_leaf->get_size() < fast_leaf->get_max_size() - 1) {
                fast_leaf->insert(key, value);
                mark_node_dirty(fast_leaf, page_lsn);
                fast_leaf_page_no = fast_leaf->get_page_no();
                inserted = true;
            }
        } catch (...) {
            buffer_pool_manager_->unpin_page(fast_leaf->page, false);
            delete fast_leaf;
            throw;
        }
        buffer_pool_manager_->unpin_page(fast_leaf->page, inserted);
        delete fast_leaf;

        if (inserted) {
            if (flush_header && !IndexHeaderDirtyFlushEnabled()) {
                flush_file_hdr_for_lsn(page_lsn);
            }
            return fast_leaf_page_no;
        }
        // shared_tree_lock 离开作用域后，再走原有结构写独占协议，避免锁升级死锁。
    }

    std::scoped_lock lock{root_latch_};
    const auto header_before = CaptureDynamicHeaderState(*file_hdr_);

    IxNodeHandle *leaf = nullptr;
    const char *ticket_key = nullptr;
    if (insert_ticket != nullptr && insert_ticket->valid_) {
        ticket_key = insert_ticket->key_len_ <= IxInsertLeafTicket::kInlineKeyBytes
                         ? insert_ticket->inline_key_.data()
                         : insert_ticket->overflow_key_.data();
    }
    const bool ticket_matches = insert_ticket != nullptr && insert_ticket->valid_ &&
                                insert_ticket->owner_ == this &&
                                insert_ticket->routing_epoch_ == structural_epoch_ &&
                                insert_ticket->key_len_ == file_hdr_->col_tot_len_ &&
                                memcmp(ticket_key, key, file_hdr_->col_tot_len_) == 0 &&
                                insert_ticket->leaf_page_no_ >= IX_INIT_ROOT_PAGE &&
                                insert_ticket->leaf_page_no_ < file_hdr_->num_pages_;
    if (insert_ticket != nullptr) {
        // 凭据只能消费一次；无论命中还是回退，后续调用都不得复用。
        insert_ticket->valid_ = false;
    }
    if (ticket_matches) {
        leaf = fetch_node(insert_ticket->leaf_page_no_);
        if (!leaf->is_leaf_page()) {
            buffer_pool_manager_->unpin_page(leaf->page, false);
            delete leaf;
            leaf = nullptr;
        }
    }
    if (leaf == nullptr) {
        leaf = find_leaf_page(key, Operation::INSERT, transaction).first;
    }
    try {
        leaf->insert(key, value);
    } catch (...) {
        // leaf 已经被 fetch_node pin 住；重复 key 抛错时也必须释放，避免缓冲池页面泄漏
        buffer_pool_manager_->unpin_page(leaf->page, false);
        delete leaf;
        throw;
    }

    // 叶节点键值对数量达到上限（btree_order+1），需要分裂
    if (leaf->get_size() >= leaf->get_max_size()) {
        IxNodeHandle *new_node = split(leaf, page_lsn);
        // 将分裂产生的 new_node 的最小 key 上推到父节点
        insert_into_parent(leaf, new_node->get_key(0), new_node, transaction, page_lsn);
        mark_node_dirty(new_node, page_lsn);
        buffer_pool_manager_->unpin_page(new_node->page, true);
        delete new_node;
    }

    page_id_t leaf_page_no = leaf->get_page_no();
    mark_node_dirty(leaf, page_lsn);
    buffer_pool_manager_->unpin_page(leaf->page, true);
    delete leaf;
    // 普通叶页写只修改索引页；文件头仅在分裂、根变化或空闲链变化时才需要同步。
    // 开关关闭时保留原有逐次刷新行为，便于使用同一二进制做完整 A/B。
    const bool header_changed = !(header_before == CaptureDynamicHeaderState(*file_hdr_));
    if (flush_header && (!IndexHeaderDirtyFlushEnabled() || header_changed)) {
        flush_file_hdr_for_lsn(page_lsn);
    }
    return leaf_page_no;
}

/**
 * @brief 用于删除B+树中含有指定key的键值对
 * @param key 要删除的key值
 * @param transaction 事务指针
 */
bool IxIndexHandle::delete_entry(const char *key, Transaction *transaction, lsn_t page_lsn, bool flush_header) {
    std::scoped_lock lock{root_latch_};
    const auto header_before = CaptureDynamicHeaderState(*file_hdr_);

    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::DELETE, transaction);
    if (leaf == nullptr) {
        return false;
    }

    int old_size = leaf->get_size();
    bool removes_first_key = old_size > 0 &&
                             ix_compare(leaf->get_key(0), key, file_hdr_->col_types_, file_hdr_->col_lens_) == 0;
    leaf->remove(key);
    bool deleted = (leaf->get_size() < old_size);
    if (deleted && removes_first_key) {
        // 叶页最小键变化会更新父分隔键；旧凭据不能再绕过完整树下降。
        BumpRoutingEpochLocked();
    }

    if (deleted) {
        // 删除后若节点键数低于最小值，触发合并或重分配
        coalesce_or_redistribute(leaf, transaction, nullptr, page_lsn);
    }

    mark_node_dirty(leaf, page_lsn);
    buffer_pool_manager_->unpin_page(leaf->page, true);
    delete leaf;
    const bool header_changed = !(header_before == CaptureDynamicHeaderState(*file_hdr_));
    if (deleted && flush_header && (!IndexHeaderDirtyFlushEnabled() || header_changed)) {
        flush_file_hdr_for_lsn(page_lsn);
    }
    return deleted;
}

/**
 * @brief 用于处理合并和重分配的逻辑，用于删除键值对后调用
 *
 * @param node 执行完删除操作的结点
 * @param transaction 事务指针
 * @param root_is_latched 传出参数：根节点是否上锁，用于并发操作
 * @return 是否需要删除结点
 * @note User needs to first find the sibling of input page.
 * If sibling's size + input page's size >= 2 * page's minsize, then redistribute.
 * Otherwise, merge(Coalesce).
 */
bool IxIndexHandle::coalesce_or_redistribute(IxNodeHandle *node, Transaction *transaction, bool *root_is_latched,
                                             lsn_t page_lsn) {
    BumpRoutingEpochLocked();
    // 句柄所有权约定：node 由调用者负责 unpin+delete；本函数只负责清理自己 fetch 出来的 parent / neighbor。
    // 被合并掉的页通过 release_node_handle 挂入空闲链表（不再 delete_page），页号高水位保持不变。

    // 根节点允许键数量低于 min_size，单独处理
    if (node->is_root_page()) {
        return adjust_root(node, page_lsn);
    }

    // 键数量充足，无需调整
    if (node->get_size() >= node->get_min_size()) {
        // 更新父节点中该节点对应的 key（可能是第一个 key 变了）
        maintain_parent(node, page_lsn);
        return false;
    }

    IxNodeHandle *parent = fetch_node(node->get_parent_page_no());
    int idx = parent->find_child(node);
    int neighbor_idx = (idx > 0) ? idx - 1 : idx + 1;
    IxNodeHandle *neighbor = fetch_node(parent->value_at(neighbor_idx));

    if (node->get_size() + neighbor->get_size() >= 2 * node->get_min_size()) {
        // 键数量之和足够维持两个节点，只需从兄弟借一个键值对，node 与 neighbor 都保留
        redistribute(neighbor, node, parent, idx, page_lsn);
        mark_node_dirty(neighbor, page_lsn);
        buffer_pool_manager_->unpin_page(neighbor->page, true);
        delete neighbor;
        mark_node_dirty(parent, page_lsn);
        buffer_pool_manager_->unpin_page(parent->page, true);
        delete parent;
        return false;
    }

    // 合并：始终把右侧节点并入左侧节点，右侧节点的页回收进空闲链表
    IxNodeHandle *left = (idx > 0) ? neighbor : node;
    IxNodeHandle *right = (idx > 0) ? node : neighbor;

    int move_num = right->get_size();
    left->insert_pairs(left->get_size(), right->get_key(0), right->get_rid(0), move_num);

    if (right->is_leaf_page()) {
        // 叶节点：维护双向链表，并在必要时更新 last_leaf
        erase_leaf(right, page_lsn);
        if (file_hdr_->last_leaf_ == right->get_page_no()) {
            file_hdr_->last_leaf_ = left->get_page_no();
        }
    } else {
        // 内部节点：被移过来的孩子需要更新父指针
        for (int i = left->get_size() - move_num; i < left->get_size(); i++) {
            maintain_child(left, i, page_lsn);
        }
    }

    // 从父节点删除指向 right 的项
    parent->erase_pair(parent->find_child(right));
    // right 此时仍被 pin，写入 next_free_page_no 后随后续 unpin(dirty) 落盘
    release_node_handle(*right);

    bool node_merged = (right == node);

    // neighbor 句柄由本函数 fetch，无论它是 left 还是 right，都在此 unpin+delete；
    // 若 neighbor 就是被合并掉的 right，unpin(dirty) 会持久化它的空闲链表指针
    mark_node_dirty(neighbor, page_lsn);
    buffer_pool_manager_->unpin_page(neighbor->page, true);
    delete neighbor;

    // 父节点少了一个 key，可能继续触发合并/重分配；parent 仍由本函数持有，递归后统一清理
    coalesce_or_redistribute(parent, transaction, root_is_latched, page_lsn);
    mark_node_dirty(parent, page_lsn);
    buffer_pool_manager_->unpin_page(parent->page, true);
    delete parent;

    // 返回调用者传入的 node 是否已被合并删除（node 句柄与 unpin 仍由调用者处理）
    return node_merged;
}

/**
 * @brief 用于当根结点被删除了一个键值对之后的处理
 * @param old_root_node 原根节点
 * @return bool 根结点是否需要被删除
 * @note size of root page can be less than min size and this method is only called within coalesce_or_redistribute()
 */
bool IxIndexHandle::adjust_root(IxNodeHandle *old_root_node, lsn_t page_lsn) {
    BumpRoutingEpochLocked();
    if (!old_root_node->is_leaf_page() && old_root_node->get_size() == 1) {
        // 内部根节点只剩一个孩子，直接将孩子提升为新根，降低树高
        page_id_t child_page_no = old_root_node->remove_and_return_only_child();
        update_root_page_no(child_page_no);
        IxNodeHandle *child = fetch_node(child_page_no);
        child->set_parent_page_no(IX_NO_PAGE);
        mark_node_dirty(child, page_lsn);
        buffer_pool_manager_->unpin_page(child->page, true);
        delete child;
        // 旧根页回收进空闲链表（old_root_node 仍由调用者持有 pin，随后 unpin(dirty) 落盘）
        release_node_handle(*old_root_node);
        return true;  // 原根节点需要被删除
    }
    if (old_root_node->is_leaf_page() && old_root_node->get_size() == 0) {
        // 空树仍保留这个叶子根节点，后续 insert/lower_bound 可以继续从合法 root 开始
        return false;
    }
    return false;  // 根节点无需调整
}

/**
 * @brief 重新分配node和兄弟结点neighbor_node的键值对
 * Redistribute key & value pairs from one page to its sibling page. If index == 0, move sibling page's first key
 * & value pair into end of input "node", otherwise move sibling page's last key & value pair into head of input "node".
 *
 * @param neighbor_node sibling page of input "node"
 * @param node input from method coalesceOrRedistribute()
 * @param parent the parent of "node" and "neighbor_node"
 * @param index node在parent中的rid_idx
 * @note node是之前刚被删除过一个key的结点
 * index=0，则neighbor是node后继结点，表示：node(left)      neighbor(right)
 * index>0，则neighbor是node前驱结点，表示：neighbor(left)  node(right)
 * 注意更新parent结点的相关kv对
 */
void IxIndexHandle::redistribute(IxNodeHandle *neighbor_node, IxNodeHandle *node, IxNodeHandle *parent, int index,
                                 lsn_t page_lsn) {
    BumpRoutingEpochLocked();
    if (index == 0) {
        // neighbor 在 node 右侧：从 neighbor 借第一个键值对追加到 node 末尾
        node->insert_pair(node->get_size(), neighbor_node->get_key(0), *neighbor_node->get_rid(0));
        neighbor_node->erase_pair(0);
        // 父节点中 neighbor 对应的 key 需要更新为 neighbor 的新首 key
        int neighbor_idx_in_parent = parent->find_child(neighbor_node);
        memcpy(parent->get_key(neighbor_idx_in_parent), neighbor_node->get_key(0), file_hdr_->col_tot_len_);
        // 若是内部节点，被移动的孩子的父指针需更新
        if (!node->is_leaf_page()) {
            maintain_child(node, node->get_size() - 1, page_lsn);
        }
    } else {
        // neighbor 在 node 左侧：从 neighbor 借最后一个键值对插入到 node 开头
        int last = neighbor_node->get_size() - 1;
        node->insert_pair(0, neighbor_node->get_key(last), *neighbor_node->get_rid(last));
        neighbor_node->erase_pair(last);
        // 父节点中 node 对应的 key 更新为 node 的新首 key
        int node_idx_in_parent = parent->find_child(node);
        memcpy(parent->get_key(node_idx_in_parent), node->get_key(0), file_hdr_->col_tot_len_);
        // 若是内部节点，更新被移动孩子的父指针
        if (!node->is_leaf_page()) {
            maintain_child(node, 0, page_lsn);
        }
    }
}

/**
 * @brief 这里把iid转换成了rid，即iid的slot_no作为node的rid_idx(key_idx)
 * node其实就是把slot_no作为键值对数组的下标
 * 换而言之，每个iid对应的索引槽存了一对(key,rid)，指向了(要建立索引的属性首地址,插入/删除记录的位置)
 *
 * @param iid
 * @return Rid
 * @note iid和rid存的不是一个东西，rid是上层传过来的记录位置，iid是索引内部生成的索引槽位置
 */
Rid IxIndexHandle::get_rid(const Iid &iid) const {
    std::shared_lock<std::shared_mutex> tree_lock(root_latch_);
    IxNodeHandle *node = fetch_node(iid.page_no);
    Rid rid;
    try {
        PageReadGuard node_guard(node->page);
        if (iid.slot_no < 0 || iid.slot_no >= node->get_size()) {
            throw IndexEntryNotFoundError();
        }
        rid = *node->get_rid(iid.slot_no);
    } catch (...) {
        buffer_pool_manager_->unpin_page(node->page, false);
        delete node;
        throw;
    }
    buffer_pool_manager_->unpin_page(node->page, false);
    delete node;  // fetch_node 中 new 出来的句柄，必须释放，否则索引扫描会持续涨内存
    return rid;
}

/**
 * @brief FindLeafPage + lower_bound
 *
 * @param key
 * @return Iid
 * @note 上层传入的key本来是int类型，通过(const char *)&key进行了转换
 * 可用*(int *)key转换回去
 */
Iid IxIndexHandle::lower_bound_locked(const char *key) const {
    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::FIND, nullptr);
    Iid iid;
    {
        PageReadGuard leaf_guard(leaf->page);
        int slot = leaf->lower_bound(key);
        if (slot == leaf->get_size() && leaf->get_next_leaf() != IX_LEAF_HEADER_PAGE) {
            iid = {leaf->get_next_leaf(), 0};
        } else {
            iid = {leaf->get_page_no(), slot};
        }
    }
    buffer_pool_manager_->unpin_page(leaf->page, false);
    delete leaf;
    return iid;
}

Iid IxIndexHandle::lower_bound(const char *key) const {
    std::shared_lock<std::shared_mutex> tree_lock(root_latch_);
    return lower_bound_locked(key);
}

/**
 * @brief FindLeafPage + upper_bound
 *
 * @param key
 * @return Iid
 */
Iid IxIndexHandle::upper_bound_locked(const char *key) const {
    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::FIND, nullptr);
    Iid iid;
    {
        PageReadGuard leaf_guard(leaf->page);
        int slot = leaf->upper_bound(key);
        // 若 upper_bound 落在当前叶节点末尾且不是最后一个叶节点，
        // 则推进到下一个叶节点的起始位置（与 IxScan::next 的遍历语义一致）
        if (slot == leaf->get_size() && leaf->get_next_leaf() != IX_LEAF_HEADER_PAGE) {
            iid = {leaf->get_next_leaf(), 0};
        } else {
            iid = {leaf->get_page_no(), slot};
        }
    }
    buffer_pool_manager_->unpin_page(leaf->page, false);
    delete leaf;
    return iid;
}

Iid IxIndexHandle::upper_bound(const char *key) const {
    std::shared_lock<std::shared_mutex> tree_lock(root_latch_);
    return upper_bound_locked(key);
}

bool IxIndexHandle::get_bound_key(const char *key, bool strict_upper, std::vector<char> *result) const {
    if (result == nullptr) {
        throw InternalError("Index bound key output must not be null");
    }

    // 结构变化由 root_latch_ 独占保护；这里只在一次定位和拷贝期间持有共享锁，
    // 不把锁跨越到后续子区间扫描，避免扩大 B+ 树并发协议的影响面。
    std::shared_lock<std::shared_mutex> tree_lock(root_latch_);
    auto [leaf, root_is_latched] = find_leaf_page(key, Operation::FIND, nullptr);
    bool valid = false;
    try {
        PageReadGuard leaf_guard(leaf->page);
        int slot = strict_upper ? leaf->upper_bound(key) : leaf->lower_bound(key);
        if (slot < leaf->get_size()) {
            result->assign(leaf->get_key(slot), leaf->get_key(slot) + file_hdr_->col_tot_len_);
            valid = true;
        } else {
            const page_id_t next_leaf = leaf->get_next_leaf();
            if (next_leaf != IX_LEAF_HEADER_PAGE && next_leaf != IX_NO_PAGE) {
                IxNodeHandle *next = nullptr;
                try {
                    // 保持当前叶读锁并按叶链方向锁住下一页，避免定位与拷键之间被并发插入移动边界。
                    next = fetch_node(next_leaf);
                    {
                        PageReadGuard next_guard(next->page);
                        if (next->is_leaf_page() && next->get_size() > 0) {
                            result->assign(next->get_key(0), next->get_key(0) + file_hdr_->col_tot_len_);
                            valid = true;
                        }
                    }
                } catch (...) {
                    if (next != nullptr) {
                        buffer_pool_manager_->unpin_page(next->page, false);
                        delete next;
                    }
                    throw;
                }
                buffer_pool_manager_->unpin_page(next->page, false);
                delete next;
            }
        }
    } catch (...) {
        buffer_pool_manager_->unpin_page(leaf->page, false);
        delete leaf;
        throw;
    }
    buffer_pool_manager_->unpin_page(leaf->page, false);
    delete leaf;
    return valid;
}

/**
 * @brief 指向最后一个叶子的最后一个结点的后一个
 * 用处在于可以作为IxScan的最后一个
 *
 * @return Iid
 */
Iid IxIndexHandle::leaf_end() const {
    std::shared_lock<std::shared_mutex> tree_lock(root_latch_);
    IxNodeHandle *node = fetch_node(file_hdr_->last_leaf_);
    Iid iid;
    {
        PageReadGuard node_guard(node->page);
        iid = {.page_no = file_hdr_->last_leaf_, .slot_no = node->get_size()};
    }
    buffer_pool_manager_->unpin_page(node->page, false);  // unpin it!
    delete node;
    return iid;
}

/**
 * @brief 指向第一个叶子的第一个结点
 * 用处在于可以作为IxScan的第一个
 *
 * @return Iid
 */
Iid IxIndexHandle::leaf_begin() const {
    std::shared_lock<std::shared_mutex> tree_lock(root_latch_);
    Iid iid = {.page_no = file_hdr_->first_leaf_, .slot_no = 0};
    return iid;
}

/**
 * @brief 获取一个指定结点
 *
 * @param page_no
 * @return IxNodeHandle*
 * @note pin the page, remember to unpin it outside!
 */
IxNodeHandle *IxIndexHandle::fetch_node(int page_no) const {
    Page *page = buffer_pool_manager_->fetch_page(PageId{fd_, page_no});
    IxNodeHandle *node = new IxNodeHandle(file_hdr_, page);
    
    return node;
}

/**
 * @brief 创建一个新结点
 *
 * @return IxNodeHandle*
 * @note pin the page, remember to unpin it outside!
 * 注意：对于Index的处理是，删除某个页面后，认为该被删除的页面是free_page
 * 而first_free_page实际上就是最新被删除的页面，初始为IX_NO_PAGE
 * 在最开始插入时，一直是create node，那么first_page_no一直没变，一直是IX_NO_PAGE
 * 与Record的处理不同，Record将未插入满的记录页认为是free_page
 */
IxNodeHandle *IxIndexHandle::create_node() {
    // 优先复用空闲链表中的页：避免页号无限增长，也避免重启后 set_fd2pageno(num_pages_)
    // 页号回退、覆盖仍然存活的高页号节点
    if (file_hdr_->first_free_page_no_ != IX_NO_PAGE) {
        PageId reuse_page_id = {.fd = fd_, .page_no = file_hdr_->first_free_page_no_};
        Page *page = buffer_pool_manager_->fetch_page(reuse_page_id);
        IxNodeHandle *node = new IxNodeHandle(file_hdr_, page);
        // 弹出空闲链表头
        file_hdr_->first_free_page_no_ = node->page_hdr->next_free_page_no;
        return node;
    }

    // 没有空闲页时才扩展文件；num_pages_ 作为页号高水位，重启后据此初始化分配计数器
    file_hdr_->num_pages_++;
    PageId new_page_id = {.fd = fd_, .page_no = INVALID_PAGE_ID};
    Page *page = buffer_pool_manager_->new_page(&new_page_id);
    return new IxNodeHandle(file_hdr_, page);
}

/**
 * @brief 从node开始更新其父节点的第一个key，一直向上更新直到根节点
 *
 * @param node
 */
void IxIndexHandle::maintain_parent(IxNodeHandle *node, lsn_t page_lsn) {
    page_id_t child_page_no = node->get_page_no();
    page_id_t parent_page_no = node->get_parent_page_no();
    std::vector<char> child_first_key(file_hdr_->col_tot_len_);
    memcpy(child_first_key.data(), node->get_key(0), file_hdr_->col_tot_len_);

    while (parent_page_no != IX_NO_PAGE) {
        IxNodeHandle *parent = fetch_node(parent_page_no);
        int rank = 0;
        while (rank < parent->get_size() && parent->value_at(rank) != child_page_no) {
            rank++;
        }
        assert(rank < parent->get_size());

        char *parent_key = parent->get_key(rank);
        if (memcmp(parent_key, child_first_key.data(), file_hdr_->col_tot_len_) == 0) {
            [[maybe_unused]] bool parent_unpinned_clean =
                buffer_pool_manager_->unpin_page(parent->page, false);
            assert(parent_unpinned_clean);
            delete parent;
            break;
        }

        memcpy(parent_key, child_first_key.data(), file_hdr_->col_tot_len_);
        bool need_update_grandparent = (rank == 0);
        child_page_no = parent->get_page_no();
        parent_page_no = parent->get_parent_page_no();
        if (need_update_grandparent) {
            memcpy(child_first_key.data(), parent->get_key(0), file_hdr_->col_tot_len_);
        }

        mark_node_dirty(parent, page_lsn);
        [[maybe_unused]] bool parent_unpinned_dirty =
            buffer_pool_manager_->unpin_page(parent->page, true);
        assert(parent_unpinned_dirty);
        delete parent;
        if (!need_update_grandparent) {
            break;
        }
    }
}

/**
 * @brief 要删除leaf之前调用此函数，更新leaf前驱结点的next指针和后继结点的prev指针
 *
 * @param leaf 要删除的leaf
 */
void IxIndexHandle::erase_leaf(IxNodeHandle *leaf, lsn_t page_lsn) {
    assert(leaf->is_leaf_page());

    IxNodeHandle *prev = fetch_node(leaf->get_prev_leaf());
    prev->set_next_leaf(leaf->get_next_leaf());
    mark_node_dirty(prev, page_lsn);
    buffer_pool_manager_->unpin_page(prev->page, true);
    delete prev;

    IxNodeHandle *next = fetch_node(leaf->get_next_leaf());
    next->set_prev_leaf(leaf->get_prev_leaf());  // 注意此处是SetPrevLeaf()
    mark_node_dirty(next, page_lsn);
    buffer_pool_manager_->unpin_page(next->page, true);
    delete next;
}

/**
 * @brief 删除node时，更新file_hdr_.num_pages
 *
 * @param node
 */
void IxIndexHandle::release_node_handle(IxNodeHandle &node) {
    // 把被删除的页挂到空闲链表头，供 create_node 复用；页号不回退（num_pages_ 维持高水位）。
    // 调用方需保证 node 当前仍被 pin，并在之后 unpin(dirty)，使 next_free_page_no 落盘。
    node.page_hdr->next_free_page_no = file_hdr_->first_free_page_no_;
    file_hdr_->first_free_page_no_ = node.get_page_no();
}

/**
 * @brief 将node的第child_idx个孩子结点的父节点置为node
 */
void IxIndexHandle::maintain_child(IxNodeHandle *node, int child_idx, lsn_t page_lsn) {
    if (!node->is_leaf_page()) {
        //  Current node is inner node, load its child and set its parent to current node
        int child_page_no = node->value_at(child_idx);
        IxNodeHandle *child = fetch_node(child_page_no);
        child->set_parent_page_no(node->get_page_no());
        mark_node_dirty(child, page_lsn);
        buffer_pool_manager_->unpin_page(child->page, true);
        delete child;
    }
}

void IxIndexHandle::mark_node_dirty(IxNodeHandle *node, lsn_t page_lsn) {
    if (node == nullptr) {
        return;
    }
    if (page_lsn != INVALID_LSN) {
        node->page->set_page_lsn(page_lsn);
    } else {
        // load、恢复重建以及 commit 已持久化后的延迟索引删除没有新的
        // WAL 依赖；显式标记，避免写回层把漏设 LSN 的普通 DML 静默放行。
        node->page->mark_wal_free_dirty();
    }
}

void IxIndexHandle::flush_file_hdr() {
    std::vector<char> data(file_hdr_->tot_len_);
    file_hdr_->serialize(data.data());
    disk_manager_->write_page(fd_, IX_FILE_HDR_PAGE, data.data(), file_hdr_->tot_len_);
}

void IxIndexHandle::reset_to_empty() {
    // recovery 在开放连接前调用；独占根锁同时把这一前置条件固化在接口内。
    std::scoped_lock lock{root_latch_};
    BumpRoutingEpochLocked();
    release_resident_root();
    // reset 随后会从固定页号重新分配整棵树；必须逐出该 fd 的全部旧 frame，
    // 不能只处理旧根和叶头，否则复用 page 3+ 时可能命中旧树缓存副本。
    if (!buffer_pool_manager_->discard_all_pages(fd_)) {
        throw InternalError("Pinned index page during recovery reset");
    }
    file_hdr_->first_free_page_no_ = IX_NO_PAGE;
    file_hdr_->num_pages_ = IX_INIT_NUM_PAGES;
    file_hdr_->root_page_ = IX_INIT_ROOT_PAGE;
    file_hdr_->first_leaf_ = IX_INIT_ROOT_PAGE;
    file_hdr_->last_leaf_ = IX_INIT_ROOT_PAGE;
    file_hdr_->update_tot_len();
    flush_file_hdr();

    char page_buf[PAGE_SIZE];
    memset(page_buf, 0, PAGE_SIZE);
    auto *leaf_header = reinterpret_cast<IxPageHdr *>(page_buf);
    *leaf_header = {
        .next_free_page_no = IX_NO_PAGE,
        .parent = IX_NO_PAGE,
        .num_key = 0,
        .is_leaf = true,
        .prev_leaf = IX_INIT_ROOT_PAGE,
        .next_leaf = IX_INIT_ROOT_PAGE,
    };
    disk_manager_->write_page(fd_, IX_LEAF_HEADER_PAGE, page_buf, PAGE_SIZE);

    memset(page_buf, 0, PAGE_SIZE);
    auto *root_header = reinterpret_cast<IxPageHdr *>(page_buf);
    *root_header = {
        .next_free_page_no = IX_NO_PAGE,
        .parent = IX_NO_PAGE,
        .num_key = 0,
        .is_leaf = true,
        .prev_leaf = IX_LEAF_HEADER_PAGE,
        .next_leaf = IX_LEAF_HEADER_PAGE,
    };
    disk_manager_->write_page(fd_, IX_INIT_ROOT_PAGE, page_buf, PAGE_SIZE);
    disk_manager_->set_fd2pageno(fd_, file_hdr_->num_pages_);
}

void IxIndexHandle::flush_file_hdr_for_lsn(lsn_t page_lsn) {
    if (page_lsn == INVALID_LSN) {
        return;
    }
    buffer_pool_manager_->flush_log_up_to_lsn(page_lsn);
    flush_file_hdr();
}
