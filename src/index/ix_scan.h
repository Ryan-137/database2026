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

#include <vector>

#include "ix_defs.h"
#include "ix_index_handle.h"

// class IxIndexHandle;

// 用于遍历叶子结点
// 用于直接遍历叶子结点，而不用findleafpage来得到叶子结点
// 扫描生命周期持有树结构共享锁，并对当前叶页持有读锁。
class IxScan : public RecScan {
    const IxIndexHandle *ih_;
    std::shared_lock<std::shared_mutex> tree_lock_;
    Iid iid_;  // 初始为lower（用于遍历的指针）
    Iid end_;  // 初始为upper
    BufferPoolManager *bpm_;
    std::vector<char> upper_key_;
    bool key_bounded_{false};
    bool ended_{false};
    IxNodeHandle *node_;  // 当前 iid_ 所在叶页，缓存后避免 rid()/next() 每行重复 fetch
    std::unique_ptr<PageReadGuard> node_guard_;

    void latch_current_node();
    void release_current_node();
    bool current_key_exceeds_upper() const;

   public:
    IxScan(const IxIndexHandle *ih, const Iid &lower, const Iid &upper, BufferPoolManager *bpm);
    IxScan(const IxIndexHandle *ih, const char *lower_key, const char *upper_key, BufferPoolManager *bpm);
    ~IxScan() override;
    void next() override;

    bool is_end() const override { return ended_ || (!key_bounded_ && iid_ == end_); }

    Rid rid() const override;

    const Iid &iid() const { return iid_; }
};
