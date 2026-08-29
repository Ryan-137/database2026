/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "parser_api.h"

#include "parser_defs.h"

namespace parser {

int Parse(const char *sql, std::shared_ptr<ast::TreeNode> *result,
          std::string *error_msg) {
    if (result == nullptr) {
        if (error_msg != nullptr) {
            *error_msg = "parser result output is null";
        }
        return -1;
    }

    yyscan_t scanner = nullptr;
    result->reset();
    if (error_msg != nullptr) {
        error_msg->clear();
    }
    if (yylex_init(&scanner) != 0) {
        if (error_msg != nullptr) {
            *error_msg = "failed to initialize lexer";
        }
        return -1;
    }

    YY_BUFFER_STATE buf = yy_scan_string(sql == nullptr ? "" : sql, scanner);
    int rc = yyparse(scanner, result, error_msg);
    yy_delete_buffer(buf, scanner);
    yylex_destroy(scanner);
    if (rc != 0) {
        result->reset();
    }
    return rc;
}

}
