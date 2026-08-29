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

#include <list>
#include <functional>
#include <mutex>  
#include <vector>

#include "common/config.h"
#include "replacer/replacer.h"
#include "unordered_map"

/*
LRUReplacer实现了LRU替换策略
*/
class LRUReplacer : public Replacer {
   public:
    /**
     * @description: 创建一个新的LRUReplacer
     * @param {size_t} num_pages LRUReplacer最多需要存储的page数量
     */
    explicit LRUReplacer(size_t num_pages);

    ~LRUReplacer();

    bool victim(frame_id_t *frame_id);

    // 只在 LRU 尾部的有界窗口内寻找满足条件的 victim。这样可以优先使用
    // 已经 clean 的冷页，又不会为了规避一次写回而淘汰远端热点页。
    bool victim_if(frame_id_t *frame_id, const std::function<bool(frame_id_t)> &predicate,
                   size_t max_scan);

    // 返回从最冷到较热的候选快照，不改变 LRU 状态。后台 cleaner 用它优先
    // 预清理即将被淘汰的 dirty 页。
    std::vector<frame_id_t> cold_candidates(size_t max_count);

    void pin(frame_id_t frame_id);

    void unpin(frame_id_t frame_id);

    // cleaner 清出的页仍是最冷候选，放回 LRU 尾部，避免一次临时写回
    // 把它错误提升为热点，导致前台仍同步淘汰后面的 dirty 页。
    void unpin_cold(frame_id_t frame_id);

    size_t Size();

   private:
    std::mutex latch_;                  // 互斥锁
    std::list<frame_id_t> LRUlist_;     // 按加入的时间顺序存放unpinned pages的frame id，首部表示最近被访问
    std::unordered_map<frame_id_t, std::list<frame_id_t>::iterator> LRUhash_;   // frame_id_t -> unpinned pages的frame id
    size_t max_size_;   // 最大容量（与缓冲池的容量相同）
};
