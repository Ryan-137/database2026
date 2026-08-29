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
#include "common/common.h"
#include "parser/ast.h"

#include "parser/parser.h"
#include "system/sm.h"

typedef enum PlanTag{
    T_Invalid = 1,
    T_Help,
    T_ShowTable,
    T_ShowIndex,
    T_DescTable,
    T_CreateTable,
    T_CreateStaticCheckpoint,
    T_DropTable,
    T_CreateIndex,
    T_DropIndex,
    T_SetKnob,
    T_SetTransactionIsolation,
    T_Insert,
    T_Update,
    T_Delete,
    T_select,
    T_Transaction_begin,
    T_Transaction_commit,
    T_Transaction_abort,
    T_Transaction_rollback,
    T_SeqScan,
    T_IndexScan,
    T_NestLoop,
    T_IndexNestedLoop,
    T_SortMerge,    // sort merge join
    T_Sort,
    T_Aggregate,
    T_Union,
    T_Projection,
    T_ExplainAnalyze
} PlanTag;

// 查询执行计划
class Plan
{
public:
    PlanTag tag;
    virtual ~Plan() = default;
};

//自底向上顺序
class ScanPlan : public Plan
{
    public:
        ScanPlan(PlanTag tag, SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds,
                 std::vector<std::string> index_col_names, std::string real_tab_name = "", bool skip_scan = false)
        {
            Plan::tag = tag;
            tab_name_ = std::move(tab_name);
            real_tab_name_ = real_tab_name.empty() ? tab_name_ : std::move(real_tab_name);
            conds_ = std::move(conds);
            TabMeta &tab = sm_manager->db_.get_table(real_tab_name_);
            cols_ = tab.cols;
            for (auto &col : cols_) col.tab_name = tab_name_;
            len_ = cols_.back().offset + cols_.back().len;
            fed_conds_ = conds_;
            index_col_names_ = index_col_names;//indexscan会用的，index信息
            skip_scan_ = skip_scan;
        
        }
        ~ScanPlan(){}
        // 以下变量同ScanExecutor中的变量
        std::string tab_name_;
        std::string real_tab_name_;
        std::vector<ColMeta> cols_;                
        std::vector<Condition> conds_;             
        size_t len_;                               
        std::vector<Condition> fed_conds_;
        std::vector<std::string> index_col_names_;
        bool skip_scan_{false};
    
};

class JoinPlan : public Plan
{
    public:
        JoinPlan(PlanTag tag, std::shared_ptr<Plan> left, std::shared_ptr<Plan> right, std::vector<Condition> conds)
        {
            Plan::tag = tag;
            left_ = std::move(left);
            right_ = std::move(right);
            conds_ = std::move(conds);
            type = INNER_JOIN;
        }
        ~JoinPlan(){}
        // 左节点
        std::shared_ptr<Plan> left_;
        // 右节点
        std::shared_ptr<Plan> right_;
        // 连接条件
        std::vector<Condition> conds_;
        // future TODO: 后续可以支持的连接类型
        JoinType type;
};

class ProjectionPlan : public Plan
{
    public:
        ProjectionPlan(PlanTag tag, std::shared_ptr<Plan> subplan, std::vector<TabCol> sel_cols)
        {
            Plan::tag = tag;
            subplan_ = std::move(subplan);
            sel_cols_ = std::move(sel_cols);
        }
        ~ProjectionPlan(){}
        std::shared_ptr<Plan> subplan_;
        std::vector<TabCol> sel_cols_;
        
};

class AggregatePlan : public Plan
{
    public:
        AggregatePlan(std::shared_ptr<Plan> subplan, std::vector<SelectExpr> select_exprs,
                      std::vector<TabCol> group_cols, std::vector<HavingCondition> having_conds)
            : subplan_(std::move(subplan)), select_exprs_(std::move(select_exprs)),
              group_cols_(std::move(group_cols)), having_conds_(std::move(having_conds)) {
            Plan::tag = T_Aggregate;
        }
        std::shared_ptr<Plan> subplan_;
        std::vector<SelectExpr> select_exprs_;
        std::vector<TabCol> group_cols_;
        std::vector<HavingCondition> having_conds_;
};

class IndexNestedLoopJoinPlan : public Plan
{
    public:
        IndexNestedLoopJoinPlan(std::shared_ptr<Plan> left, std::string right_table, std::string right_real_table,
                                std::vector<Condition> right_conds, std::vector<Condition> join_conds,
                                TabCol outer_key, TabCol inner_key)
            : left_(std::move(left)), right_table_(std::move(right_table)),
              right_real_table_(std::move(right_real_table)),
              right_conds_(std::move(right_conds)), join_conds_(std::move(join_conds)),
              outer_key_(std::move(outer_key)), inner_key_(std::move(inner_key)) {
            Plan::tag = T_IndexNestedLoop;
        }
        std::shared_ptr<Plan> left_;
        std::string right_table_;
        std::string right_real_table_;
        std::vector<Condition> right_conds_;
        std::vector<Condition> join_conds_;
        TabCol outer_key_;
        TabCol inner_key_;
};

class UnionPlan : public Plan
{
    public:
        UnionPlan(std::vector<std::shared_ptr<Plan>> subplans, std::vector<ColMeta> output_cols)
            : subplans_(std::move(subplans)), output_cols_(std::move(output_cols)) {
            Plan::tag = T_Union;
        }
        std::vector<std::shared_ptr<Plan>> subplans_;
        std::vector<ColMeta> output_cols_;
};

class SortPlan : public Plan
{
    public:
        SortPlan(PlanTag tag, std::shared_ptr<Plan> subplan, std::vector<TabCol> sel_cols,
                 std::vector<bool> is_desc, int limit)
        {
            Plan::tag = tag;
            subplan_ = std::move(subplan);
            sel_cols_ = std::move(sel_cols);
            is_desc_ = std::move(is_desc);
            limit_ = limit;
        }
        ~SortPlan(){}
        std::shared_ptr<Plan> subplan_;
        std::vector<TabCol> sel_cols_;
        std::vector<bool> is_desc_;
        int limit_;

};

class ExplainPlan : public Plan
{
    public:
        explicit ExplainPlan(std::vector<std::string> lines)
            : lines_(std::move(lines)) {
            Plan::tag = T_ExplainAnalyze;
        }

        std::vector<std::string> lines_;
};

// dml语句，包括insert; delete; update; select语句　
class DMLPlan : public Plan
{
    public:
        DMLPlan(PlanTag tag, std::shared_ptr<Plan> subplan,std::string tab_name,
                std::vector<Value> values, std::vector<Condition> conds,
                std::vector<SetClause> set_clauses)
        {
            Plan::tag = tag;
            subplan_ = std::move(subplan);
            tab_name_ = std::move(tab_name);
            values_ = std::move(values);
            conds_ = std::move(conds);
            set_clauses_ = std::move(set_clauses);
        }
        ~DMLPlan(){}
        std::shared_ptr<Plan> subplan_;
        std::string tab_name_;
        std::vector<Value> values_;
        std::vector<Condition> conds_;
        std::vector<SetClause> set_clauses_;
        std::vector<TabCol> result_cols_;
        bool expect_existing_unique_point_{false};
};

// ddl语句, 包括create/drop table; create/drop index;
class DDLPlan : public Plan
{
    public:
        DDLPlan(PlanTag tag, std::string tab_name, std::vector<std::string> col_names, std::vector<ColDef> cols)
        {
            Plan::tag = tag;
            tab_name_ = std::move(tab_name);
            cols_ = std::move(cols);
            tab_col_names_ = std::move(col_names);
        }
        ~DDLPlan(){}
        std::string tab_name_;
        std::vector<std::string> tab_col_names_;
        std::vector<ColDef> cols_;
};

// help; show tables; desc tables; begin; abort; commit; rollback语句对应的plan
class OtherPlan : public Plan
{
    public:
        OtherPlan(PlanTag tag, std::string tab_name)
        {
            Plan::tag = tag;
            tab_name_ = std::move(tab_name);            
        }
        ~OtherPlan(){}
        std::string tab_name_;
};

// Set Knob Plan
class SetKnobPlan : public Plan
{
    public:
        SetKnobPlan(ast::SetKnobType knob_type, bool bool_value) {
            Plan::tag = T_SetKnob;
            set_knob_type_ = knob_type;
            bool_value_ = bool_value;
        }
    ast::SetKnobType set_knob_type_;
    bool bool_value_;
};

class SetTransactionIsolationPlan : public Plan
{
    public:
        explicit SetTransactionIsolationPlan(IsolationLevel isolation_level) {
            Plan::tag = T_SetTransactionIsolation;
            isolation_level_ = isolation_level;
        }
    IsolationLevel isolation_level_;
};

class plannerInfo{
    public:
    std::shared_ptr<ast::SelectStmt> parse;
    std::vector<Condition> where_conds;
    std::vector<TabCol> sel_cols;
    std::shared_ptr<Plan> plan;
    std::vector<std::shared_ptr<Plan>> table_scan_executors;
    std::vector<SetClause> set_clauses;
    plannerInfo(std::shared_ptr<ast::SelectStmt> parse_):parse(std::move(parse_)){}

};
