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
#include <cstring>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class ProjectionExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;        // 投影节点的儿子节点
    std::vector<ColMeta> cols_;                     // 需要投影的字段
    size_t len_;                                    // 字段总长度
    std::vector<size_t> sel_idxs_;
    std::unique_ptr<RmRecord> current_tuple_;

    void materialize_current() {
        current_tuple_.reset();
        if (prev_->is_end()) {
            return;
        }
        std::unique_ptr<RmRecord> owned_prev;
        const RmRecord *prev_record = prev_->CurrentOrNext(&owned_prev);
        if (prev_record == nullptr) {
            return;
        }
        current_tuple_ = std::make_unique<RmRecord>(static_cast<int>(len_));
        const auto &prev_cols = prev_->cols();
        for (size_t i = 0; i < sel_idxs_.size(); i++) {
            const auto &src_col = prev_cols[sel_idxs_[i]];
            const auto &dst_col = cols_[i];
            memcpy(current_tuple_->data + dst_col.offset, prev_record->data + src_col.offset, src_col.len);
        }
    }

   public:
    ProjectionExecutor(std::unique_ptr<AbstractExecutor> prev, const std::vector<TabCol> &sel_cols) {
        prev_ = std::move(prev);

        size_t curr_offset = 0;
        auto &prev_cols = prev_->cols();
        for (auto &sel_col : sel_cols) {
            auto pos = get_col(prev_cols, sel_col);
            sel_idxs_.push_back(pos - prev_cols.begin());
            auto col = *pos;
            col.offset = curr_offset;
            curr_offset += col.len;
            cols_.push_back(col);
        }
        len_ = curr_offset;
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    std::string getType() override { return "ProjectionExecutor"; }

    void beginTuple() override {
        prev_->beginTuple();
        materialize_current();
    }

    void nextTuple() override {
        prev_->nextTuple();
        materialize_current();
    }

    bool is_end() const override { return prev_->is_end(); }

    std::unique_ptr<RmRecord> Next() override {
        return current_tuple_ == nullptr ? nullptr : std::make_unique<RmRecord>(*current_tuple_);
    }

    const RmRecord *Current() const override { return current_tuple_.get(); }

    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }

    Rid &rid() override { return prev_->rid(); }
};
