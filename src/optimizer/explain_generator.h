#pragma once

#include <memory>
#include <string>
#include <vector>

#include "parser/ast.h"
#include "system/sm.h"

std::vector<std::string> make_rmdb_explain_lines(const std::shared_ptr<ast::SelectStmt> &select,
                                                 SmManager *sm_manager);
