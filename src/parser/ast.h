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

#include <vector>
#include <string>
#include <memory>
#include "common/common.h"

enum JoinType {
    INNER_JOIN, LEFT_JOIN, RIGHT_JOIN, FULL_JOIN
};
namespace ast {

struct SelectStmt;
struct UnionStmt;

enum SvType {
    SV_TYPE_INT, SV_TYPE_FLOAT, SV_TYPE_STRING, SV_TYPE_BOOL
};

enum SvCompOp {
    SV_OP_EQ, SV_OP_NE, SV_OP_LT, SV_OP_GT, SV_OP_LE, SV_OP_GE
};

enum OrderByDir {
    OrderBy_DEFAULT,
    OrderBy_ASC,
    OrderBy_DESC
};

enum SetKnobType {
    EnableNestLoop, EnableSortMerge
};

enum class TxnIsolationLevel {
    SNAPSHOT_ISOLATION,
    SERIALIZABLE
};

// Base class for tree nodes
struct TreeNode {
    virtual ~TreeNode() = default;  // enable polymorphism
};

struct Help : public TreeNode {
};

struct ShowTables : public TreeNode {
};

struct ShowIndex : public TreeNode{
    std::string tab_name;

    ShowIndex(std::string tab_name_) : tab_name(std::move(tab_name_)) {}
};

struct TxnBegin : public TreeNode {
};

struct TxnCommit : public TreeNode {
};

struct TxnAbort : public TreeNode {
};

struct TxnRollback : public TreeNode {
};

struct TypeLen : public TreeNode {
    SvType type;
    int len;

    TypeLen(SvType type_, int len_) : type(type_), len(len_) {}
};

struct Field : public TreeNode {
};

struct ColDef : public Field {
    std::string col_name;
    std::shared_ptr<TypeLen> type_len;

    ColDef(std::string col_name_, std::shared_ptr<TypeLen> type_len_) :
            col_name(std::move(col_name_)), type_len(std::move(type_len_)) {}
};

struct CreateTable : public TreeNode {
    std::string tab_name;
    std::vector<std::shared_ptr<Field>> fields;

    CreateTable(std::string tab_name_, std::vector<std::shared_ptr<Field>> fields_) :
            tab_name(std::move(tab_name_)), fields(std::move(fields_)) {}
};

struct CreateStaticCheckpoint : public TreeNode {
};

struct DropTable : public TreeNode {
    std::string tab_name;

    DropTable(std::string tab_name_) : tab_name(std::move(tab_name_)) {}
};

struct DescTable : public TreeNode {
    std::string tab_name;

    DescTable(std::string tab_name_) : tab_name(std::move(tab_name_)) {}
};

struct CreateIndex : public TreeNode {
    std::string tab_name;
    std::vector<std::string> col_names;

    CreateIndex(std::string tab_name_, std::vector<std::string> col_names_) :
            tab_name(std::move(tab_name_)), col_names(std::move(col_names_)) {}
};

struct DropIndex : public TreeNode {
    std::string tab_name;
    std::vector<std::string> col_names;

    DropIndex(std::string tab_name_, std::vector<std::string> col_names_) :
            tab_name(std::move(tab_name_)), col_names(std::move(col_names_)) {}
};

struct Expr : public TreeNode {
};

struct Value : public Expr {
};

struct IntLit : public Value {
    int val;

    IntLit(int val_) : val(val_) {}
};

struct FloatLit : public Value {
    float val;

    FloatLit(float val_) : val(val_) {}
};

struct StringLit : public Value {
    std::string val;

    StringLit(std::string val_) : val(std::move(val_)) {}
};

struct BoolLit : public Value {
    bool val;

    BoolLit(bool val_) : val(val_) {}
};

struct Col : public Expr {
    std::string tab_name;
    std::string col_name;
    bool is_aggregate = false;
    AggFunc agg_func = AGG_COUNT;
    bool count_star = false;
    std::string alias;

    Col(std::string tab_name_, std::string col_name_) :
            tab_name(std::move(tab_name_)), col_name(std::move(col_name_)) {}

    Col(AggFunc agg_func_, std::string tab_name_, std::string col_name_, bool count_star_) :
            tab_name(std::move(tab_name_)), col_name(std::move(col_name_)), is_aggregate(true),
            agg_func(agg_func_), count_star(count_star_) {}
};

enum class UpdateExprKind {
    VALUE,
    COLUMN,
    ADD,
    SUB
};

struct UpdateExpr : public TreeNode {
    UpdateExprKind kind;
    std::shared_ptr<Value> value;
    std::shared_ptr<Col> column;
    std::shared_ptr<UpdateExpr> lhs;
    std::shared_ptr<UpdateExpr> rhs;

    explicit UpdateExpr(std::shared_ptr<Value> value_)
            : kind(UpdateExprKind::VALUE), value(std::move(value_)) {}

    explicit UpdateExpr(std::shared_ptr<Col> column_)
            : kind(UpdateExprKind::COLUMN), column(std::move(column_)) {}

    UpdateExpr(UpdateExprKind kind_, std::shared_ptr<UpdateExpr> lhs_, std::shared_ptr<UpdateExpr> rhs_)
            : kind(kind_), lhs(std::move(lhs_)), rhs(std::move(rhs_)) {}
};

struct SetClause : public TreeNode {
    std::string col_name;
    std::shared_ptr<UpdateExpr> rhs;

    SetClause(std::string col_name_, std::shared_ptr<UpdateExpr> rhs_) :
            col_name(std::move(col_name_)), rhs(std::move(rhs_)) {}
};

struct BinaryExpr : public TreeNode {
    std::shared_ptr<Col> lhs;
    SvCompOp op;
    std::shared_ptr<Expr> rhs;

    BinaryExpr(std::shared_ptr<Col> lhs_, SvCompOp op_, std::shared_ptr<Expr> rhs_) :
            lhs(std::move(lhs_)), op(op_), rhs(std::move(rhs_)) {}
};

struct OrderBy : public TreeNode
{
    std::shared_ptr<Col> cols;
    OrderByDir orderby_dir;
    OrderBy( std::shared_ptr<Col> cols_, OrderByDir orderby_dir_) :
       cols(std::move(cols_)), orderby_dir(std::move(orderby_dir_)) {}
};

struct InsertStmt : public TreeNode {
    std::string tab_name;
    std::vector<std::shared_ptr<Value>> vals;

    InsertStmt(std::string tab_name_, std::vector<std::shared_ptr<Value>> vals_) :
            tab_name(std::move(tab_name_)), vals(std::move(vals_)) {}
};

struct DeleteStmt : public TreeNode {
    std::string tab_name;
    std::vector<std::shared_ptr<BinaryExpr>> conds;

    DeleteStmt(std::string tab_name_, std::vector<std::shared_ptr<BinaryExpr>> conds_) :
            tab_name(std::move(tab_name_)), conds(std::move(conds_)) {}
};

struct UpdateStmt : public TreeNode {
    std::string tab_name;
    std::vector<std::shared_ptr<SetClause>> set_clauses;
    std::vector<std::shared_ptr<BinaryExpr>> conds;

    UpdateStmt(std::string tab_name_,
               std::vector<std::shared_ptr<SetClause>> set_clauses_,
               std::vector<std::shared_ptr<BinaryExpr>> conds_) :
            tab_name(std::move(tab_name_)), set_clauses(std::move(set_clauses_)), conds(std::move(conds_)) {}
};

struct JoinExpr : public TreeNode {
    std::string left;
    std::string right;
    std::vector<std::shared_ptr<BinaryExpr>> conds;
    JoinType type;

    JoinExpr(std::string left_, std::string right_,
               std::vector<std::shared_ptr<BinaryExpr>> conds_, JoinType type_) :
            left(std::move(left_)), right(std::move(right_)), conds(std::move(conds_)), type(type_) {}
};

struct TableRef : public TreeNode {
    std::string tab_name;  // 真实表名，用于系统元数据查找
    std::string alias;     // SQL 中的别名；没有别名时为空

    std::shared_ptr<UnionStmt> union_query;

    TableRef(std::string tab_name_, std::string alias_) :
            tab_name(std::move(tab_name_)), alias(std::move(alias_)) {}

    TableRef(std::shared_ptr<UnionStmt> union_query_, std::string alias_) :
            alias(std::move(alias_)), union_query(std::move(union_query_)) {}

    std::string visible_name() const { return alias.empty() ? tab_name : alias; }
    bool is_derived_union() const { return union_query != nullptr; }
};

struct FromClause : public TreeNode {
    std::vector<std::shared_ptr<TableRef>> tables;
    std::vector<std::shared_ptr<BinaryExpr>> join_conds;

    FromClause() = default;

    explicit FromClause(std::shared_ptr<TableRef> table) {
        tables.push_back(std::move(table));
    }
};

struct SelectStmt : public TreeNode {
    std::vector<std::shared_ptr<Col>> cols;
    std::vector<std::string> tabs;
    std::vector<std::shared_ptr<TableRef>> table_refs;
    std::vector<std::shared_ptr<BinaryExpr>> conds;
    std::vector<std::shared_ptr<BinaryExpr>> where_conds;
    std::vector<std::shared_ptr<BinaryExpr>> join_conds;
    std::vector<std::shared_ptr<JoinExpr>> jointree;

    
    std::vector<std::shared_ptr<Col>> group_cols;
    std::vector<std::shared_ptr<BinaryExpr>> having_conds;
    bool has_sort = false;
    std::vector<std::shared_ptr<OrderBy>> orders;
    int limit = -1;


    SelectStmt(std::vector<std::shared_ptr<Col>> cols_,
               std::vector<std::string> tabs_,
               std::vector<std::shared_ptr<BinaryExpr>> conds_,
               std::vector<std::shared_ptr<OrderBy>> orders_, int limit_ = -1) :
            cols(std::move(cols_)), tabs(std::move(tabs_)), conds(std::move(conds_)),
            where_conds(conds),
            orders(std::move(orders_)), limit(limit_) {
                for (auto &tab : tabs) {
                    table_refs.push_back(std::make_shared<TableRef>(tab, ""));
                }
                has_sort = !orders.empty();
            }

    SelectStmt(std::vector<std::shared_ptr<Col>> cols_,
               std::shared_ptr<FromClause> from_,
               std::vector<std::shared_ptr<BinaryExpr>> where_conds_,
               std::vector<std::shared_ptr<Col>> group_cols_,
               std::vector<std::shared_ptr<BinaryExpr>> having_conds_,
               std::vector<std::shared_ptr<OrderBy>> orders_, int limit_) :
            cols(std::move(cols_)), table_refs(std::move(from_->tables)),
            where_conds(std::move(where_conds_)), join_conds(std::move(from_->join_conds)),
            group_cols(std::move(group_cols_)), having_conds(std::move(having_conds_)),
            orders(std::move(orders_)), limit(limit_) {
                for (auto &table_ref : table_refs) {
                    tabs.push_back(table_ref->tab_name);
                }
                conds = join_conds;
                conds.insert(conds.end(), where_conds.begin(), where_conds.end());
                has_sort = !orders.empty();
            }
};

struct UnionStmt : public TreeNode {
    std::vector<std::shared_ptr<SelectStmt>> branches;

    UnionStmt(std::shared_ptr<SelectStmt> lhs, std::shared_ptr<SelectStmt> rhs) {
        branches.push_back(std::move(lhs));
        branches.push_back(std::move(rhs));
    }
};

struct ExplainAnalyzeStmt : public TreeNode {
    std::shared_ptr<SelectStmt> select;  // EXPLAIN ANALYZE 只包裹 SELECT 查询

    explicit ExplainAnalyzeStmt(std::shared_ptr<SelectStmt> select_) : select(std::move(select_)) {}
};

// set enable_nestloop
struct SetStmt : public TreeNode {
    SetKnobType set_knob_type_;
    bool bool_val_;

    SetStmt(SetKnobType &type, bool bool_value) : 
        set_knob_type_(type), bool_val_(bool_value) { }
};

struct SetTransactionIsolationStmt : public TreeNode {
    TxnIsolationLevel isolation;

    explicit SetTransactionIsolationStmt(TxnIsolationLevel isolation_) : isolation(isolation_) {}
};

// Semantic value
struct SemValue {
    int sv_int;
    float sv_float;
    std::string sv_str;
    bool sv_bool;
    OrderByDir sv_orderby_dir;
    std::vector<std::string> sv_strs;

    std::shared_ptr<TreeNode> sv_node;

    SvCompOp sv_comp_op;

    std::shared_ptr<TypeLen> sv_type_len;

    std::shared_ptr<Field> sv_field;
    std::vector<std::shared_ptr<Field>> sv_fields;

    std::shared_ptr<Expr> sv_expr;

    std::shared_ptr<Value> sv_val;
    std::vector<std::shared_ptr<Value>> sv_vals;

    std::shared_ptr<Col> sv_col;
    std::vector<std::shared_ptr<Col>> sv_cols;

    std::shared_ptr<TableRef> sv_table_ref;
    std::shared_ptr<FromClause> sv_from_clause;
    std::shared_ptr<UnionStmt> sv_union;

    std::shared_ptr<SetClause> sv_set_clause;
    std::vector<std::shared_ptr<SetClause>> sv_set_clauses;
    std::shared_ptr<UpdateExpr> sv_update_expr;

    std::shared_ptr<BinaryExpr> sv_cond;
    std::vector<std::shared_ptr<BinaryExpr>> sv_conds;

    std::shared_ptr<OrderBy> sv_orderby;
    std::vector<std::shared_ptr<OrderBy>> sv_orderbys;
    AggFunc sv_agg_func;

    SetKnobType sv_setKnobType;
    TxnIsolationLevel sv_isolation_level;
};

}

#define YYSTYPE ast::SemValue
