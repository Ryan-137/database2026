/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "rm_scan.h"
#include "rm_file_handle.h"

/**
 * @brief 初始化file_handle和rid
 * @param file_handle
 */
RmScan::RmScan(const RmFileHandle *file_handle) : file_handle_(file_handle) {
    // Snapshot the monotonically increasing page boundary without taking
    // heap_mutex_: concurrent inserts may append new pages after this scan
    // starts, but under SI those new tuples either have commit_ts greater than
    // the transaction snapshot or are still uncommitted, so skipping newly
    // appended pages does not make an old snapshot miss a visible version.
    num_pages_snapshot_ = file_handle_->num_pages_snapshot();
    // 初始定位到第一个数据页的"哨兵"位置（slot_no = -1），
    // 再调用 next() 推进到第一条真实记录
    rid_ = {RM_FIRST_RECORD_PAGE, -1};
    next();
}

/**
 * @brief 找到文件中下一个存放了记录的位置
 */
void RmScan::next() {
    // 从当前 rid_ 的下一个槽位开始，逐页逐槽扫描 bitmap
    for (int page_no = rid_.page_no;
         page_no < num_pages_snapshot_;
         page_no++) {
        RmPageHandle page_handle = file_handle_->fetch_page_handle(page_no);
        // 当前页内的起始搜索位置：当前页从 slot_no+1 开始，新页从 0 开始
        int start_slot = (page_no == rid_.page_no) ? rid_.slot_no : -1;
        int slot;
        {
            PageReadGuard page_guard(page_handle.page);
            slot = Bitmap::next_bit(true, page_handle.bitmap,
                                    file_handle_->file_hdr_.num_records_per_page,
                                    start_slot);
        }
        file_handle_->buffer_pool_manager_->unpin_page(page_handle.page, false);

        if (slot < file_handle_->file_hdr_.num_records_per_page) {
            // 找到有效记录
            rid_ = {page_no, slot};
            return;
        }
    }
    // 扫描结束，标记为 end 状态
    rid_ = {RM_NO_PAGE, -1};
}

/**
 * @brief ​ 判断是否到达文件末尾
 */
bool RmScan::is_end() const {
    // RM_NO_PAGE 作为哨兵值，标志扫描已遍历所有记录
    return rid_.page_no == RM_NO_PAGE;
}

/**
 * @brief RmScan内部存放的rid
 */
Rid RmScan::rid() const {
    return rid_;
}
