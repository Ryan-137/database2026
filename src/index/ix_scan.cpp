/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "ix_scan.h"

IxScan::IxScan(const IxIndexHandle *ih, const Iid &lower, const Iid &upper, BufferPoolManager *bpm)
    : ih_(ih), tree_lock_(ih_->root_latch_), iid_(lower), end_(upper), bpm_(bpm), node_(nullptr) {
    if (!is_end()) {
        node_ = ih_->fetch_node(iid_.page_no);
        latch_current_node();
        assert(node_->is_leaf_page());
        while (!is_end() && iid_.slot_no >= node_->get_size()) {
            if (iid_.page_no == ih_->file_hdr_->last_leaf_) {
                iid_ = end_;
                release_current_node();
                break;
            }
            page_id_t next_leaf = node_->get_next_leaf();
            release_current_node();
            iid_.page_no = next_leaf;
            iid_.slot_no = 0;
            if (!is_end()) {
                node_ = ih_->fetch_node(iid_.page_no);
                latch_current_node();
                assert(node_->is_leaf_page());
            }
        }
    }
}

IxScan::IxScan(const IxIndexHandle *ih, const char *lower_key, const char *upper_key, BufferPoolManager *bpm)
    : ih_(ih), tree_lock_(ih_->root_latch_), iid_(ih_->lower_bound_locked(lower_key)), end_(iid_), bpm_(bpm),
      upper_key_(upper_key, upper_key + ih_->file_hdr_->col_tot_len_), key_bounded_(true), node_(nullptr) {
    if (iid_.page_no == IX_LEAF_HEADER_PAGE || iid_.page_no == IX_NO_PAGE) {
        ended_ = true;
        return;
    }
    node_ = ih_->fetch_node(iid_.page_no);
    latch_current_node();
    if (!node_->is_leaf_page() || iid_.slot_no < 0 || iid_.slot_no >= node_->get_size()) {
        ended_ = true;
        release_current_node();
        return;
    }
    if (current_key_exceeds_upper()) {
        ended_ = true;
        release_current_node();
    }
}

IxScan::~IxScan() {
    release_current_node();
}

void IxScan::latch_current_node() {
    if (node_ != nullptr) {
        node_guard_ = std::make_unique<PageReadGuard>(node_->page);
    }
}

void IxScan::release_current_node() {
    if (node_ != nullptr) {
        node_guard_.reset();
        bpm_->unpin_page(node_->page, false);
        delete node_;
        node_ = nullptr;
    }
}

bool IxScan::current_key_exceeds_upper() const {
    return key_bounded_ && node_ != nullptr &&
           ix_compare(node_->get_key(iid_.slot_no), upper_key_.data(),
                      ih_->file_hdr_->col_types_, ih_->file_hdr_->col_lens_) > 0;
}

/**
 * @brief 
 * @todo 加上读锁（需要使用缓冲池得到page）
 */
void IxScan::next() {
    if (is_end() || node_ == nullptr || !node_->is_leaf_page() ||
        iid_.page_no != node_->get_page_no() || iid_.slot_no < 0 || iid_.slot_no >= node_->get_size()) {
        throw InternalError("Invalid B+ tree range scan position");
    }

    iid_.slot_no++;
    if (!key_bounded_ && is_end()) {
        release_current_node();
        return;
    }
    if (iid_.slot_no == node_->get_size()) {
        page_id_t next_leaf = node_->get_next_leaf();
        if (next_leaf == IX_LEAF_HEADER_PAGE || next_leaf == IX_NO_PAGE) {
            ended_ = true;
            release_current_node();
            return;
        }
        release_current_node();
        iid_.slot_no = 0;
        iid_.page_no = next_leaf;
        if (!key_bounded_ && is_end()) {
            return;
        }
        node_ = ih_->fetch_node(iid_.page_no);
        latch_current_node();
        if (!node_->is_leaf_page() || node_->get_size() <= 0) {
            release_current_node();
            throw InternalError("Invalid B+ tree leaf chain");
        }
    }
    if (current_key_exceeds_upper()) {
        ended_ = true;
        release_current_node();
    }
}

Rid IxScan::rid() const {
    if (is_end() || node_ == nullptr || iid_.page_no != node_->get_page_no() ||
        iid_.slot_no < 0 || iid_.slot_no >= node_->get_size()) {
        throw InternalError("Invalid B+ tree range scan position");
    }
    return *node_->get_rid(iid_.slot_no);
}
