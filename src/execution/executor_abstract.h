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

#include "execution_common.h"
#include "execution_defs.h"
#include "common/common.h"
#include "index/ix.h"
#include "system/sm.h"

enum class ScanExecutionMode { VISIBLE_TUPLE, CANDIDATE_RID };

class AbstractExecutor {
   public:
    Rid _abstract_rid;

    Context *context_;

    virtual ~AbstractExecutor() = default;

    virtual size_t tupleLen() const { return 0; };

    virtual const std::vector<ColMeta> &cols() const {
        std::vector<ColMeta> *_cols = nullptr;
        return *_cols;
    };

    virtual std::string getType() { return "AbstractExecutor"; };

    virtual void beginTuple(){};

    virtual void nextTuple(){};

    virtual bool is_end() const { return true; };

    virtual Rid &rid() = 0;

    virtual std::unique_ptr<RmRecord> Next() = 0;

    // 只读视图在下一次 beginTuple/nextTuple 前有效；默认算子没有稳定缓存。
    virtual const RmRecord *Current() const { return nullptr; }

    const RmRecord *CurrentOrNext(std::unique_ptr<RmRecord> *owned) {
        const RmRecord *current = Current();
        if (current != nullptr) {
            return current;
        }
        *owned = Next();
        return owned->get();
    }

    virtual ColMeta get_col_offset(const TabCol &target) { return ColMeta();};

    std::vector<ColMeta>::const_iterator get_col(const std::vector<ColMeta> &rec_cols, const TabCol &target) const {
        auto pos = std::find_if(rec_cols.begin(), rec_cols.end(), [&](const ColMeta &col) {
            return col.tab_name == target.tab_name && col.name == target.col_name;
        });
        if (pos == rec_cols.end()) {
            throw ColumnNotFoundError(target.tab_name + '.' + target.col_name);
        }
        return pos;
    }

    void bind_conds(const std::vector<ColMeta> &rec_cols, std::vector<Condition> *conds) const {
        for (auto &cond : *conds) {
            auto lhs_col = get_col(rec_cols, cond.lhs_col);
            cond.bound_lhs_offset = lhs_col->offset;
            cond.bound_type = lhs_col->type;
            cond.bound_len = lhs_col->len;
            if (cond.is_rhs_val) {
                if (cond.rhs_val.raw == nullptr) {
                    cond.rhs_val.init_raw(lhs_col->len);
                }
                cond.bound_rhs_offset = -1;
            } else {
                cond.bound_rhs_offset = get_col(rec_cols, cond.rhs_col)->offset;
            }
        }
    }

    // 判断一条记录是否满足单个条件；join 后的拼接记录同样可用
    bool eval_cond(const std::vector<ColMeta> &rec_cols, const RmRecord *record, const Condition &cond) {
        const bool bound = cond.bound_lhs_offset >= 0;
        auto lhs_col = bound ? rec_cols.end() : get_col(rec_cols, cond.lhs_col);
        const int lhs_offset = bound ? cond.bound_lhs_offset : lhs_col->offset;
        const ColType lhs_type = bound ? cond.bound_type : lhs_col->type;
        const int lhs_len = bound ? cond.bound_len : lhs_col->len;
        const char *lhs = record->data + lhs_offset;
        const char *rhs;

        if (cond.is_rhs_val) {
            rhs = cond.rhs_val.raw->data;
        } else {
            int rhs_offset = bound ? cond.bound_rhs_offset : get_col(rec_cols, cond.rhs_col)->offset;
            rhs = record->data + rhs_offset;
        }

        int cmp = CompareValue(lhs, rhs, lhs_type, lhs_len);
        return CheckOp(cmp, cond.op);
    }

    // 多个条件之间是 AND 关系，任意一个不满足则过滤
    bool eval_conds(const std::vector<ColMeta> &rec_cols, const RmRecord *record,
                    const std::vector<Condition> &conds) {
        for (auto &cond : conds) {
            if (!eval_cond(rec_cols, record, cond)) {
                return false;
            }
        }
        return true;
    }

    // 从当前迭代器位置推进到下一条满足条件的记录，并把命中记录返回给调用方缓存，
    // 避免上层 Next() 再次 get_record 重复读页/拷贝；未命中（扫描结束）返回 nullptr
    template <typename GetRecord>
    std::unique_ptr<RmRecord> advance_to_next_valid(RecScan *scan, Rid *rid, const std::vector<ColMeta> &rec_cols,
                               const std::vector<Condition> &conds, GetRecord get_record) {
        while (!scan->is_end()) {
            *rid = scan->rid();
            auto record = get_record(*rid);
            if (record == nullptr) {
                scan->next();
                continue;
            }
            if (eval_conds(rec_cols, record.get(), conds)) {
                return record;
            }
            scan->next();
        }
        return nullptr;
    }
};
