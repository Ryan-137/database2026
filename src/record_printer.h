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
#include <iostream>
#include <iomanip>
#include <string>
#include <sstream>
#include "common/context.h"
#include "common/config.h"

class RecordPrinter {
    static constexpr size_t COL_WIDTH = 16;
    // 为省略标记、记录数和结尾 NUL 预留空间，保持框架原有 8 KiB 响应边界。
    static constexpr size_t RESPONSE_TAIL_RESERVE = 40;
    size_t num_cols;
public:
    RecordPrinter(size_t num_cols_) : num_cols(num_cols_) {
        assert(num_cols_ > 0);
    }

    void print_separator(Context *context) const {
        for (size_t i = 0; i < num_cols; i++) {
            context->TryAppendTableResponse("+" + std::string(COL_WIDTH + 2, '-'), RESPONSE_TAIL_RESERVE);
        }
        context->TryAppendTableResponse("+\n", RESPONSE_TAIL_RESERVE);
    }

    void print_record(const std::vector<std::string> &rec_str, Context *context) const {
        assert(rec_str.size() == num_cols);
        for (auto col: rec_str) {
            if (col.size() > COL_WIDTH) {
                col = col.substr(0, COL_WIDTH - 3) + "...";
            }
            std::stringstream ss;
            ss << "| " << std::setw(COL_WIDTH) << col << " ";
            context->TryAppendTableResponse(ss.str(), RESPONSE_TAIL_RESERVE);
        }
        context->TryAppendTableResponse("|\n", RESPONSE_TAIL_RESERVE);
    }

    static void print_record_count(size_t num_rec, Context *context) {
        if (context->ResponseTruncated()) {
            context->AppendResponse("... ...\n");
        }
        context->AppendResponse("Total record(s): " + std::to_string(num_rec) + '\n');
    }
};
