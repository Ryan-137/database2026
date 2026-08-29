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
#include <unordered_map>
#include <vector>

#include "parser/parser.h"
#include "system/sm.h"
#include "common/common.h"

class Query{
    public:
    std::shared_ptr<ast::TreeNode> parse;
    // TODO jointree
    // 所有条件，保留给原有 SELECT/UPDATE/DELETE 规划逻辑使用
    std::vector<Condition> conds;
    // WHERE 条件和 JOIN ON 条件分开保存，便于任务四做谓词下推与 Join 节点输出
    std::vector<Condition> where_conds;
    std::vector<Condition> join_conds;
    // 投影列
    std::vector<TabCol> cols;
    std::vector<SelectExpr> select_exprs;
    std::vector<TabCol> group_cols;
    std::vector<HavingCondition> having_conds;
    std::vector<OrderSpec> order_specs;
    bool has_aggregation = false;
    int limit = -1;
    bool has_union_source = false;
    std::string union_alias;
    std::vector<std::shared_ptr<Query>> union_branches;
    std::vector<ColMeta> output_cols;
    // 表名
    std::vector<std::string> tables;
    // SQL 别名与真实表名的映射：执行访问真实表名，计划输出可使用别名
    std::unordered_map<std::string, std::string> alias_to_table;
    std::unordered_map<std::string, std::string> table_to_alias;
    std::unordered_map<std::string, std::string> table_real_names;
    // update 的set 值
    std::vector<SetClause> set_clauses;
    //insert 的values值
    std::vector<Value> values;

    Query(){}

};

class Analyze
{
private:
    SmManager *sm_manager_;
public:
    Analyze(SmManager *sm_manager) : sm_manager_(sm_manager){}
    ~Analyze(){}

    std::shared_ptr<Query> do_analyze(std::shared_ptr<ast::TreeNode> root);

private:
    TabCol check_column(const std::vector<ColMeta> &all_cols, TabCol target,
                        const std::unordered_map<std::string, std::string> &alias_to_table = {});
    void get_all_cols(const std::vector<std::string> &tab_names, std::vector<ColMeta> &all_cols,
                      const std::unordered_map<std::string, std::string> &table_real_names = {});
    void get_clause(const std::vector<std::shared_ptr<ast::BinaryExpr>> &sv_conds, std::vector<Condition> &conds);
    void check_clause(const std::vector<std::string> &tab_names, std::vector<Condition> &conds,
                      const std::unordered_map<std::string, std::string> &alias_to_table = {},
                      const std::unordered_map<std::string, std::string> &table_real_names = {});
    Value convert_sv_value(const std::shared_ptr<ast::Value> &sv_val);
    UpdateExpr convert_update_expr(const std::shared_ptr<ast::UpdateExpr> &sv_expr, const std::string &tab_name,
                                   const ColMeta &target_col, const std::vector<ColMeta> &all_cols);
    CompOp convert_sv_comp_op(ast::SvCompOp op);
};

