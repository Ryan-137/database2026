/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lru_replacer.h"

#include <algorithm>

LRUReplacer::LRUReplacer(size_t num_pages) { max_size_ = num_pages; }

LRUReplacer::~LRUReplacer() = default;  

/**
 * @description: 使用LRU策略删除一个victim frame，并返回该frame的id
 * @param {frame_id_t*} frame_id 被移除的frame的id，如果没有frame被移除返回nullptr
 * @return {bool} 如果成功淘汰了一个页面则返回true，否则返回false
 */
bool LRUReplacer::victim(frame_id_t* frame_id) {
    std::scoped_lock lock{latch_};

    if (LRUlist_.empty()) {
        return false;
    }
    // 链表尾部是最久未被访问的帧，优先淘汰
    *frame_id = LRUlist_.back();
    LRUhash_.erase(*frame_id);
    LRUlist_.pop_back();
    return true;
}

bool LRUReplacer::victim_if(frame_id_t *frame_id,
                            const std::function<bool(frame_id_t)> &predicate,
                            size_t max_scan) {
    if (frame_id == nullptr || !predicate || max_scan == 0) {
        return false;
    }
    std::scoped_lock lock{latch_};
    size_t scanned = 0;
    for (auto it = LRUlist_.rbegin(); it != LRUlist_.rend() && scanned < max_scan; ++it, ++scanned) {
        if (!predicate(*it)) {
            continue;
        }
        *frame_id = *it;
        auto erase_it = std::next(it).base();
        LRUhash_.erase(*frame_id);
        LRUlist_.erase(erase_it);
        return true;
    }
    return false;
}

std::vector<frame_id_t> LRUReplacer::cold_candidates(size_t max_count) {
    std::vector<frame_id_t> result;
    if (max_count == 0) {
        return result;
    }
    std::scoped_lock lock{latch_};
    result.reserve(std::min(max_count, LRUlist_.size()));
    for (auto it = LRUlist_.rbegin(); it != LRUlist_.rend() && result.size() < max_count; ++it) {
        result.push_back(*it);
    }
    return result;
}

/**
 * @description: 固定指定的frame，即该页面无法被淘汰
 * @param {frame_id_t} 需要固定的frame的id
 */
void LRUReplacer::pin(frame_id_t frame_id) {
    std::scoped_lock lock{latch_};
    // 被 pin 的帧表示正在使用，不能被淘汰，从可淘汰集合中移除
    auto it = LRUhash_.find(frame_id);
    if (it != LRUhash_.end()) {
        LRUlist_.erase(it->second);
        LRUhash_.erase(it);
    }
}

/**
 * @description: 取消固定一个frame，代表该页面可以被淘汰
 * @param {frame_id_t} frame_id 取消固定的frame的id
 */
void LRUReplacer::unpin(frame_id_t frame_id) {
    std::scoped_lock lock{latch_};
    // 若已在可淘汰列表中则幂等返回，避免重复添加（unit_test 明确验证此行为）
    if (LRUhash_.count(frame_id)) {
        return;
    }
    // 放到链表头部表示最近访问，尾部最久未访问
    LRUlist_.push_front(frame_id);
    LRUhash_[frame_id] = LRUlist_.begin();
}

void LRUReplacer::unpin_cold(frame_id_t frame_id) {
    std::scoped_lock lock{latch_};
    if (LRUhash_.count(frame_id)) {
        return;
    }
    LRUlist_.push_back(frame_id);
    LRUhash_[frame_id] = std::prev(LRUlist_.end());
}

/**
 * @description: 获取当前replacer中可以被淘汰的页面数量
 */
size_t LRUReplacer::Size() { return LRUlist_.size(); }
