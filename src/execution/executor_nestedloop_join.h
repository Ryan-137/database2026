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

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;    // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_;   // 右儿子节点（需要join的表）
    size_t len_;                                // join后获得的每条记录的长度
    std::vector<ColMeta> cols_;                 // join后获得的记录的字段

    std::vector<Condition> fed_conds_;          // join条件
    bool isend;
    std::unique_ptr<RmRecord> current_tuple_;

    std::unique_ptr<RmRecord> make_join_record() {
        std::unique_ptr<RmRecord> owned_left;
        std::unique_ptr<RmRecord> owned_right;
        const RmRecord *left_record = left_->CurrentOrNext(&owned_left);
        const RmRecord *right_record = right_->CurrentOrNext(&owned_right);
        auto record = std::make_unique<RmRecord>(len_);
        memcpy(record->data, left_record->data, left_->tupleLen());
        memcpy(record->data + left_->tupleLen(), right_record->data, right_->tupleLen());
        return record;
    }

    // 推进到下一组满足连接条件的左右元组
    void advance_to_next_valid() {
        while (!left_->is_end()) {
            while (!right_->is_end()) {
                auto record = make_join_record();
                if (eval_conds(cols_, record.get(), fed_conds_)) {
                    current_tuple_ = std::move(record);
                    isend = false;
                    return;
                }
                right_->nextTuple();
            }
            left_->nextTuple();
            if (!left_->is_end()) {
                right_->beginTuple();
            }
        }
        current_tuple_.reset();
        isend = true;
    }

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right, 
                            std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }

        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        isend = false;
        fed_conds_ = std::move(conds);
        bind_conds(cols_, &fed_conds_);

    }

    void beginTuple() override {
        left_->beginTuple();
        if (left_->is_end()) {
            isend = true;
            return;
        }
        right_->beginTuple();
        if (right_->is_end()) {
            isend = true;
            return;
        }
        advance_to_next_valid();
    }

    void nextTuple() override {
        if (isend) {
            return;
        }
        right_->nextTuple();
        if (right_->is_end()) {
            left_->nextTuple();
            if (!left_->is_end()) {
                right_->beginTuple();
            }
        }
        advance_to_next_valid();
    }

    std::unique_ptr<RmRecord> Next() override {
        return current_tuple_ == nullptr ? nullptr : std::make_unique<RmRecord>(*current_tuple_);
    }

    const RmRecord *Current() const override { return current_tuple_.get(); }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    std::string getType() override { return "NestedLoopJoinExecutor"; }

    bool is_end() const override { return isend; }

    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }

    Rid &rid() override { return _abstract_rid; }
};
