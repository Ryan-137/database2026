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
#include <optional>
#include <vector>

#include "transaction/transaction.h"
#include "transaction/transaction_manager.h"
#include "common/common.h"

auto ReconstructTuple(const TabMeta *schema, const RmRecord &base_tuple, const TupleMeta &base_meta,
                      const std::vector<UndoLog> &undo_logs) -> std::optional<RmRecord>;


auto IsWriteWriteConflict(timestamp_t tuple_ts, Transaction *txn) -> bool;

// 按列类型比较左右两侧原始字节，返回值语义与 strcmp 一致
inline int CompareValue(const char *lhs, const char *rhs, ColType type, int len) {
    if (type == TYPE_INT) {
        int lhs_val = *reinterpret_cast<const int *>(lhs);
        int rhs_val = *reinterpret_cast<const int *>(rhs);
        return (lhs_val > rhs_val) - (lhs_val < rhs_val);
    }
    if (type == TYPE_FLOAT) {
        float lhs_val = *reinterpret_cast<const float *>(lhs);
        float rhs_val = *reinterpret_cast<const float *>(rhs);
        return (lhs_val > rhs_val) - (lhs_val < rhs_val);
    }
    return memcmp(lhs, rhs, len);
}

// 将比较结果套用到 SQL 条件运算符上
inline bool CheckOp(int cmp, CompOp op) {
    switch (op) {
        case OP_EQ: return cmp == 0;
        case OP_NE: return cmp != 0;
        case OP_LT: return cmp < 0;
        case OP_GT: return cmp > 0;
        case OP_LE: return cmp <= 0;
        case OP_GE: return cmp >= 0;
    }
    return false;
}
