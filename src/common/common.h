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

#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "defs.h"
#include "record/rm_defs.h"


struct TabCol {
    std::string tab_name;
    std::string col_name;

    friend bool operator<(const TabCol &x, const TabCol &y) {
        return std::make_pair(x.tab_name, x.col_name) < std::make_pair(y.tab_name, y.col_name);
    }
};

enum AggFunc { AGG_COUNT, AGG_MAX, AGG_MIN, AGG_SUM, AGG_AVG };

struct SelectExpr {
    bool is_aggregate = false;
    AggFunc agg_func = AGG_COUNT;
    bool count_star = false;
    TabCol col;
    std::string output_name;
};

struct Value {
    ColType type;  // type of value
    union {
        int int_val;      // int value
        float float_val;  // float value
    };
    std::string str_val;  // string value

    std::shared_ptr<RmRecord> raw;  // raw record buffer

    void set_int(int int_val_) {
        type = TYPE_INT;
        int_val = int_val_;
    }

    void set_float(float float_val_) {
        type = TYPE_FLOAT;
        float_val = float_val_;
    }

    void set_str(std::string str_val_) {
        type = TYPE_STRING;
        str_val = std::move(str_val_);
    }

    void init_raw(int len) {
        assert(raw == nullptr);
        raw = std::make_shared<RmRecord>(len);
        if (type == TYPE_INT) {
            assert(len == sizeof(int));
            *(int *)(raw->data) = int_val;
        } else if (type == TYPE_FLOAT) {
            assert(len == sizeof(float));
            *(float *)(raw->data) = float_val;
        } else if (type == TYPE_STRING) {
            if (len < (int)str_val.size()) {
                throw StringOverflowError();
            }
            memset(raw->data, 0, len);
            memcpy(raw->data, str_val.c_str(), str_val.size());
        }
    }
};

enum CompOp { OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE };

enum class UpdateExprKind { VALUE, COLUMN, ADD, SUB };

struct UpdateExpr {
    UpdateExprKind kind = UpdateExprKind::VALUE;
    Value value;
    TabCol column;
    std::shared_ptr<UpdateExpr> lhs;
    std::shared_ptr<UpdateExpr> rhs;
    // executor 构造期绑定；-1 表示尚未绑定，语义层仍以 column 为准。
    int bound_offset = -1;
    ColType bound_type = TYPE_INT;
    int bound_len = 0;
};

struct Condition {
    TabCol lhs_col;   // left-hand side column
    CompOp op;        // comparison operator
    bool is_rhs_val;  // true if right-hand side is a value (not a column)
    TabCol rhs_col;   // right-hand side column
    Value rhs_val;    // right-hand side value
    // executor 构造期绑定，避免逐行按字符串搜索列元数据。
    int bound_lhs_offset = -1;
    int bound_rhs_offset = -1;
    ColType bound_type = TYPE_INT;
    int bound_len = 0;
};

struct HavingCondition {
    SelectExpr lhs;
    CompOp op;
    bool is_rhs_val = true;
    Value rhs;
    SelectExpr rhs_expr;
};

struct OrderSpec {
    SelectExpr expr;
    bool is_desc = false;
};

struct SetClause {
    TabCol lhs;
    UpdateExpr rhs;
    int bound_target_offset = -1;
    ColType bound_target_type = TYPE_INT;
    int bound_target_len = 0;
};
