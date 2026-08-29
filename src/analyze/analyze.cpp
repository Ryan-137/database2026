/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "analyze.h"

#include <unordered_set>

namespace {
void check_table_exists(SmManager *sm_manager, const std::string &tab_name) {
    if (!sm_manager->db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }
}

void check_value_type_and_len(const ColMeta &col, Value &val) {
    if (col.type == TYPE_FLOAT && val.type == TYPE_INT) {
        val.set_float(static_cast<float>(val.int_val));
    }
    if (col.type != val.type) {
        throw IncompatibleTypeError(coltype2str(col.type), coltype2str(val.type));
    }
    if (col.type == TYPE_STRING && col.len < static_cast<int>(val.str_val.size())) {
        throw StringOverflowError();
    }
}

const char *agg_name(AggFunc func) {
    switch (func) {
        case AGG_COUNT: return "COUNT";
        case AGG_MAX: return "MAX";
        case AGG_MIN: return "MIN";
        case AGG_SUM: return "SUM";
        case AGG_AVG: return "AVG";
    }
    return "";
}

bool same_col(const TabCol &lhs, const TabCol &rhs) {
    return lhs.tab_name == rhs.tab_name && lhs.col_name == rhs.col_name;
}

std::vector<ColMeta> make_union_schema(const std::vector<std::shared_ptr<Query>> &branches,
                                       const std::string &alias) {
    if (branches.size() < 2) throw RMDBError("UNION requires at least two branches");
    std::vector<ColMeta> result = branches.front()->output_cols;
    for (size_t branch_idx = 1; branch_idx < branches.size(); ++branch_idx) {
        const auto &cols = branches[branch_idx]->output_cols;
        if (cols.size() != result.size()) {
            throw RMDBError("UNION branches must return the same number of columns");
        }
        for (size_t col_idx = 0; col_idx < result.size(); ++col_idx) {
            auto &common = result[col_idx];
            const auto &candidate = cols[col_idx];
            if (common.type == candidate.type) {
                if (common.type == TYPE_STRING) common.len = std::max(common.len, candidate.len);
            } else if ((common.type == TYPE_INT && candidate.type == TYPE_FLOAT) ||
                       (common.type == TYPE_FLOAT && candidate.type == TYPE_INT)) {
                common.type = TYPE_FLOAT;
                common.len = sizeof(float);
            } else {
                throw IncompatibleTypeError(coltype2str(common.type), coltype2str(candidate.type));
            }
        }
    }

    int offset = 0;
    for (auto &col : result) {
        col.tab_name = alias;
        col.offset = offset;
        col.index = false;
        offset += col.len;
    }
    return result;
}

std::string resolve_table_name(const std::string &name,
                               const std::unordered_map<std::string, std::string> &alias_to_table) {
    auto it = alias_to_table.find(name);
    return it == alias_to_table.end() ? name : it->second;
}

std::string real_table_name(const std::string &visible_name,
                            const std::unordered_map<std::string, std::string> &table_real_names) {
    auto it = table_real_names.find(visible_name);
    return it == table_real_names.end() ? visible_name : it->second;
}
}

/**
 * @description: 分析器，进行语义分析和查询重写，需要检查不符合语义规定的部分
 * @param {shared_ptr<ast::TreeNode>} parse parser生成的结果集
 * @return {shared_ptr<Query>} Query 
 */
std::shared_ptr<Query> Analyze::do_analyze(std::shared_ptr<ast::TreeNode> parse)
{
    std::shared_ptr<Query> query = std::make_shared<Query>();
    auto select = std::dynamic_pointer_cast<ast::SelectStmt>(parse);
    if (select != nullptr)
    {
        std::vector<ColMeta> all_cols;
        std::shared_ptr<ast::TableRef> derived_ref;
        for (auto &table_ref : select->table_refs) {
            if (table_ref->is_derived_union()) {
                if (derived_ref != nullptr || select->table_refs.size() != 1) {
                    throw RMDBError("UNION derived table must be the only FROM source");
                }
                derived_ref = table_ref;
            }
        }

        if (derived_ref != nullptr) {
            if (!select->cols.empty()) throw RMDBError("UNION derived table currently supports outer SELECT * only");
            if (!select->where_conds.empty() || !select->join_conds.empty() || !select->group_cols.empty() ||
                !select->having_conds.empty()) {
                throw RMDBError("Unsupported clause on UNION derived table");
            }
            query->has_union_source = true;
            query->union_alias = derived_ref->alias;
            for (auto &branch : derived_ref->union_query->branches) {
                query->union_branches.push_back(do_analyze(branch));
            }
            all_cols = make_union_schema(query->union_branches, query->union_alias);
            query->alias_to_table[query->union_alias] = query->union_alias;
            query->table_to_alias[query->union_alias] = query->union_alias;
            query->table_real_names[query->union_alias] = query->union_alias;
        }

        // Keep each visible relation instance distinct while retaining its physical table name.
        else if (!select->table_refs.empty()) {
            std::unordered_set<std::string> seen_instances;
            for (auto &table_ref : select->table_refs) {
                check_table_exists(sm_manager_, table_ref->tab_name);
                std::string visible_name = table_ref->visible_name();
                if (!seen_instances.insert(visible_name).second) {
                    throw RMDBError("Duplicate table alias: " + visible_name);
                }
                query->tables.push_back(visible_name);
                query->alias_to_table[visible_name] = visible_name;
                query->table_to_alias[visible_name] = visible_name;
                query->table_real_names[visible_name] = table_ref->tab_name;
            }
        } else {
            // 兼容旧 parser 产物：没有 table_refs 时仍按原 tabs 解析
            query->tables = select->tabs;
            for (auto &tab_name : query->tables) {
                check_table_exists(sm_manager_, tab_name);
                query->alias_to_table[tab_name] = tab_name;
                query->table_to_alias[tab_name] = tab_name;
                query->table_real_names[tab_name] = tab_name;
            }
        }
        // 处理target list，再target list中添加上表名，例如 a.id
        
        if (!query->has_union_source) get_all_cols(query->tables, all_cols, query->table_real_names);
        // 分开处理 WHERE 与 JOIN ON，任务四后续需要分别生成 Filter 与 Join 节点
        auto find_meta = [&](const TabCol &target) -> const ColMeta & {
            auto pos = std::find_if(all_cols.begin(), all_cols.end(), [&](const ColMeta &col) {
                return col.tab_name == target.tab_name && col.name == target.col_name;
            });
            if (pos == all_cols.end()) throw ColumnNotFoundError(target.tab_name + "." + target.col_name);
            return *pos;
        };
        auto make_select_expr = [&](const std::shared_ptr<ast::Col> &sv_col) {
            SelectExpr expr;
            expr.is_aggregate = sv_col->is_aggregate;
            expr.agg_func = sv_col->agg_func;
            expr.count_star = sv_col->count_star;
            if (!expr.count_star) {
                expr.col = check_column(all_cols, {sv_col->tab_name, sv_col->col_name}, query->alias_to_table);
            }
            if (expr.is_aggregate && !expr.count_star) {
                const ColMeta &meta = find_meta(expr.col);
                if ((expr.agg_func == AGG_SUM || expr.agg_func == AGG_AVG) && meta.type == TYPE_STRING) {
                    throw IncompatibleTypeError("numeric", coltype2str(meta.type));
                }
            }
            if (!sv_col->alias.empty()) expr.output_name = sv_col->alias;
            else if (expr.is_aggregate) {
                expr.output_name = std::string(agg_name(expr.agg_func)) + "(" +
                                   (expr.count_star ? "*" : expr.col.col_name) + ")";
            } else expr.output_name = expr.col.col_name;
            return expr;
        };

        if (select->cols.empty()) {
            for (auto &col : all_cols) {
                SelectExpr expr;
                expr.col = {col.tab_name, col.name};
                expr.output_name = col.name;
                query->select_exprs.push_back(expr);
                query->cols.push_back(expr.col);
            }
        } else {
            for (auto &sv_sel_col : select->cols) {
                auto expr = make_select_expr(sv_sel_col);
                query->has_aggregation = query->has_aggregation || expr.is_aggregate;
                if (!expr.is_aggregate) query->cols.push_back(expr.col);
                query->select_exprs.push_back(std::move(expr));
            }
        }

        int output_offset = 0;
        for (const auto &expr : query->select_exprs) {
            ColMeta output;
            output.tab_name = query->has_union_source ? query->union_alias : "";
            output.name = expr.output_name;
            output.offset = output_offset;
            output.index = false;
            if (!expr.is_aggregate) {
                const auto &source = find_meta(expr.col);
                output.type = source.type;
                output.len = source.len;
            } else if (expr.agg_func == AGG_COUNT) {
                output.type = TYPE_INT;
                output.len = sizeof(int);
            } else if (expr.agg_func == AGG_AVG) {
                output.type = TYPE_FLOAT;
                output.len = sizeof(float);
            } else {
                const auto &source = find_meta(expr.col);
                output.type = source.type;
                output.len = source.len;
            }
            output_offset += output.len;
            query->output_cols.push_back(output);
        }

        for (auto &sv_group_col : select->group_cols) {
            query->group_cols.push_back(check_column(
                all_cols, {sv_group_col->tab_name, sv_group_col->col_name}, query->alias_to_table));
        }
        query->has_aggregation = query->has_aggregation || !query->group_cols.empty() || !select->having_conds.empty();
        if (query->has_aggregation) {
            for (auto &expr : query->select_exprs) {
                if (!expr.is_aggregate && std::none_of(query->group_cols.begin(), query->group_cols.end(),
                        [&](const TabCol &group_col) { return same_col(expr.col, group_col); })) {
                    throw RMDBError("Non-aggregate SELECT column must appear in GROUP BY");
                }
            }
        }

        auto contains_aggregate = [](const std::shared_ptr<ast::BinaryExpr> &cond) {
            if (cond->lhs->is_aggregate) return true;
            auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
            return rhs_col != nullptr && rhs_col->is_aggregate;
        };
        for (auto &cond : select->where_conds) {
            if (contains_aggregate(cond)) throw RMDBError("Aggregate functions are not allowed in WHERE");
        }
        for (auto &cond : select->join_conds) {
            if (contains_aggregate(cond)) throw RMDBError("Aggregate functions are not allowed in JOIN conditions");
        }

        get_clause(select->where_conds, query->where_conds);
        check_clause(query->tables, query->where_conds, query->alias_to_table, query->table_real_names);
        get_clause(select->join_conds, query->join_conds);
        check_clause(query->tables, query->join_conds, query->alias_to_table, query->table_real_names);

        query->conds = query->join_conds;
        query->conds.insert(query->conds.end(), query->where_conds.begin(), query->where_conds.end());

        for (auto &sv_having : select->having_conds) {
            HavingCondition having;
            having.lhs = make_select_expr(sv_having->lhs);
            having.op = convert_sv_comp_op(sv_having->op);
            if (!having.lhs.is_aggregate && std::none_of(query->group_cols.begin(), query->group_cols.end(),
                    [&](const TabCol &group_col) { return same_col(having.lhs.col, group_col); })) {
                throw RMDBError("HAVING column must appear in GROUP BY");
            }
            auto expr_type = [&](const SelectExpr &expr) {
                if (expr.is_aggregate && expr.agg_func == AGG_COUNT) {
                    return std::make_pair(TYPE_INT, static_cast<int>(sizeof(int)));
                }
                if (expr.is_aggregate && expr.agg_func == AGG_AVG) {
                    return std::make_pair(TYPE_FLOAT, static_cast<int>(sizeof(float)));
                }
                const auto &meta = find_meta(expr.col);
                return std::make_pair(meta.type, meta.len);
            };
            auto lhs_meta = expr_type(having.lhs);
            if (auto rhs = std::dynamic_pointer_cast<ast::Value>(sv_having->rhs)) {
                having.rhs = convert_sv_value(rhs);
                if (lhs_meta.first == TYPE_FLOAT && having.rhs.type == TYPE_INT) {
                    having.rhs.set_float(static_cast<float>(having.rhs.int_val));
                }
                if (having.rhs.type != lhs_meta.first) {
                    throw IncompatibleTypeError(coltype2str(lhs_meta.first), coltype2str(having.rhs.type));
                }
                having.rhs.init_raw(lhs_meta.second);
            } else if (auto rhs_col = std::dynamic_pointer_cast<ast::Col>(sv_having->rhs)) {
                having.is_rhs_val = false;
                having.rhs_expr = make_select_expr(rhs_col);
                if (!having.rhs_expr.is_aggregate &&
                    std::none_of(query->group_cols.begin(), query->group_cols.end(),
                        [&](const TabCol &group_col) { return same_col(having.rhs_expr.col, group_col); })) {
                    throw RMDBError("HAVING column must appear in GROUP BY");
                }
                auto rhs_meta = expr_type(having.rhs_expr);
                if (rhs_meta.first != lhs_meta.first) {
                    throw IncompatibleTypeError(coltype2str(lhs_meta.first), coltype2str(rhs_meta.first));
                }
            } else {
                throw RMDBError("Invalid HAVING expression");
            }
            query->having_conds.push_back(std::move(having));
        }

        for (auto &sv_order : select->orders) {
            OrderSpec order;
            auto alias_match = std::find_if(query->select_exprs.begin(), query->select_exprs.end(),
                [&](const SelectExpr &expr) {
                    return !sv_order->cols->is_aggregate && sv_order->cols->tab_name.empty() &&
                           expr.output_name == sv_order->cols->col_name;
                });
            order.expr = alias_match != query->select_exprs.end() ? *alias_match : make_select_expr(sv_order->cols);
            order.is_desc = sv_order->orderby_dir == ast::OrderBy_DESC;
            query->order_specs.push_back(std::move(order));
        }
        if (select->limit < -1) throw RMDBError("LIMIT must be non-negative");
        query->limit = select->limit;
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(parse)) {
        check_table_exists(sm_manager_, x->tab_name);
        TabMeta &tab = sm_manager_->db_.get_table(x->tab_name);
        std::vector<ColMeta> all_cols = tab.cols;
        for (auto &sv_clause : x->set_clauses) {
            auto col = tab.get_col(sv_clause->col_name);
            SetClause set_clause;
            set_clause.lhs.tab_name = x->tab_name;
            set_clause.lhs.col_name = sv_clause->col_name;
            set_clause.rhs = convert_update_expr(sv_clause->rhs, x->tab_name, *col, all_cols);
            query->set_clauses.push_back(std::move(set_clause));
        }
        get_clause(x->conds, query->conds);
        check_clause({x->tab_name}, query->conds);
    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(parse)) {
        check_table_exists(sm_manager_, x->tab_name);
        //处理where条件
        get_clause(x->conds, query->conds);
        check_clause({x->tab_name}, query->conds);        
    } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(parse)) {
        check_table_exists(sm_manager_, x->tab_name);
        TabMeta &tab = sm_manager_->db_.get_table(x->tab_name);
        if (x->vals.size() != tab.cols.size()) {
            throw InvalidValueCountError();
        }
        // 处理insert 的values值
        for (size_t i = 0; i < x->vals.size(); i++) {
            Value val = convert_sv_value(x->vals[i]);
            check_value_type_and_len(tab.cols[i], val);
            query->values.push_back(std::move(val));
        }
    } else {
        // do nothing
    }
    query->parse = std::move(parse);
    return query;
}


TabCol Analyze::check_column(const std::vector<ColMeta> &all_cols, TabCol target,
                             const std::unordered_map<std::string, std::string> &alias_to_table) {
    if (target.tab_name.empty()) {
        // Table name not specified, infer table name from column name
        std::string tab_name;
        for (auto &col : all_cols) {
            if (col.name == target.col_name) {
                if (!tab_name.empty()) {
                    throw AmbiguousColumnError(target.col_name);
                }
                tab_name = col.tab_name;
            }
        }
        if (tab_name.empty()) {
            throw ColumnNotFoundError(target.col_name);
        }
        target.tab_name = tab_name;
    } else {
        std::string raw_tab_name = target.tab_name;
        target.tab_name = resolve_table_name(target.tab_name, alias_to_table);
        bool found_table = false;
        for (auto &col : all_cols) {
            if (col.tab_name != target.tab_name) {
                continue;
            }
            found_table = true;
            if (col.name == target.col_name) {
                return target;
            }
        }
        if (!found_table) {
            throw TableNotFoundError(raw_tab_name);
        }
        throw ColumnNotFoundError(raw_tab_name + "." + target.col_name);
    }
    return target;
}

void Analyze::get_all_cols(const std::vector<std::string> &tab_names, std::vector<ColMeta> &all_cols,
                           const std::unordered_map<std::string, std::string> &table_real_names) {
    for (auto &sel_tab_name : tab_names) {
        const auto &sel_tab_cols = sm_manager_->db_.get_table(real_table_name(sel_tab_name, table_real_names)).cols;
        for (auto col : sel_tab_cols) {
            col.tab_name = sel_tab_name;
            all_cols.push_back(std::move(col));
        }
    }
}

void Analyze::get_clause(const std::vector<std::shared_ptr<ast::BinaryExpr>> &sv_conds, std::vector<Condition> &conds) {
    conds.clear();
    for (auto &expr : sv_conds) {
        Condition cond;
        cond.lhs_col = {.tab_name = expr->lhs->tab_name, .col_name = expr->lhs->col_name};
        cond.op = convert_sv_comp_op(expr->op);
        if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(expr->rhs)) {
            cond.is_rhs_val = true;
            cond.rhs_val = convert_sv_value(rhs_val);
        } else if (auto rhs_col = std::dynamic_pointer_cast<ast::Col>(expr->rhs)) {
            cond.is_rhs_val = false;
            cond.rhs_col = {.tab_name = rhs_col->tab_name, .col_name = rhs_col->col_name};
        }
        conds.push_back(cond);
    }
}

void Analyze::check_clause(const std::vector<std::string> &tab_names, std::vector<Condition> &conds,
                           const std::unordered_map<std::string, std::string> &alias_to_table,
                           const std::unordered_map<std::string, std::string> &table_real_names) {
    // auto all_cols = get_all_cols(tab_names);
    std::vector<ColMeta> all_cols;
    get_all_cols(tab_names, all_cols, table_real_names);
    // Get raw values in where clause
    for (auto &cond : conds) {
        // Infer table name from column name
        cond.lhs_col = check_column(all_cols, cond.lhs_col, alias_to_table);
        if (!cond.is_rhs_val) {
            cond.rhs_col = check_column(all_cols, cond.rhs_col, alias_to_table);
        }
        TabMeta &lhs_tab = sm_manager_->db_.get_table(real_table_name(cond.lhs_col.tab_name, table_real_names));
        auto lhs_col = lhs_tab.get_col(cond.lhs_col.col_name);
        ColType lhs_type = lhs_col->type;
        ColType rhs_type;
        if (cond.is_rhs_val) {
            if (lhs_type == TYPE_FLOAT && cond.rhs_val.type == TYPE_INT) {
                cond.rhs_val.set_float(static_cast<float>(cond.rhs_val.int_val));
            }
            cond.rhs_val.init_raw(lhs_col->len);
            rhs_type = cond.rhs_val.type;
        } else {
            TabMeta &rhs_tab = sm_manager_->db_.get_table(real_table_name(cond.rhs_col.tab_name, table_real_names));
            auto rhs_col = rhs_tab.get_col(cond.rhs_col.col_name);
            rhs_type = rhs_col->type;
        }
        if (lhs_type != rhs_type) {
            throw IncompatibleTypeError(coltype2str(lhs_type), coltype2str(rhs_type));
        }
    }
}


Value Analyze::convert_sv_value(const std::shared_ptr<ast::Value> &sv_val) {
    Value val;
    if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(sv_val)) {
        val.set_int(int_lit->val);
    } else if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(sv_val)) {
        val.set_float(float_lit->val);
    } else if (auto str_lit = std::dynamic_pointer_cast<ast::StringLit>(sv_val)) {
        val.set_str(str_lit->val);
    } else {
        throw InternalError("Unexpected sv value type");
    }
    return val;
}

UpdateExpr Analyze::convert_update_expr(const std::shared_ptr<ast::UpdateExpr> &sv_expr, const std::string &tab_name,
                                        const ColMeta &target_col, const std::vector<ColMeta> &all_cols) {
    UpdateExpr expr;
    switch (sv_expr->kind) {
        case ast::UpdateExprKind::VALUE: {
            expr.kind = UpdateExprKind::VALUE;
            expr.value = convert_sv_value(sv_expr->value);
            check_value_type_and_len(target_col, expr.value);
            return expr;
        }
        case ast::UpdateExprKind::COLUMN: {
            expr.kind = UpdateExprKind::COLUMN;
            expr.column = check_column(all_cols, {sv_expr->column->tab_name, sv_expr->column->col_name});
            if (expr.column.tab_name != tab_name) {
                throw TableNotFoundError(expr.column.tab_name);
            }
            auto src_col = sm_manager_->db_.get_table(tab_name).get_col(expr.column.col_name);
            if (src_col->type != target_col.type) {
                throw IncompatibleTypeError(coltype2str(target_col.type), coltype2str(src_col->type));
            }
            return expr;
        }
        case ast::UpdateExprKind::ADD:
        case ast::UpdateExprKind::SUB: {
            expr.kind = sv_expr->kind == ast::UpdateExprKind::ADD ? UpdateExprKind::ADD : UpdateExprKind::SUB;
            expr.lhs = std::make_shared<UpdateExpr>(
                convert_update_expr(sv_expr->lhs, tab_name, target_col, all_cols));
            expr.rhs = std::make_shared<UpdateExpr>(
                convert_update_expr(sv_expr->rhs, tab_name, target_col, all_cols));
            if (expr.lhs->kind != UpdateExprKind::COLUMN || expr.rhs->kind != UpdateExprKind::VALUE) {
                throw IncompatibleTypeError("column +/- literal", "expression");
            }
            auto lhs_col = sm_manager_->db_.get_table(tab_name).get_col(expr.lhs->column.col_name);
            if (lhs_col->type != TYPE_INT && lhs_col->type != TYPE_FLOAT) {
                throw IncompatibleTypeError("numeric", coltype2str(lhs_col->type));
            }
            if (expr.rhs->value.type != TYPE_INT && expr.rhs->value.type != TYPE_FLOAT) {
                throw IncompatibleTypeError("numeric", coltype2str(expr.rhs->value.type));
            }
            if (target_col.type == TYPE_INT && (lhs_col->type != TYPE_INT || expr.rhs->value.type != TYPE_INT)) {
                throw IncompatibleTypeError(coltype2str(target_col.type), "FLOAT");
            }
            if (target_col.type != TYPE_INT && target_col.type != TYPE_FLOAT) {
                throw IncompatibleTypeError("numeric", coltype2str(target_col.type));
            }
            return expr;
        }
    }
    throw InternalError("Unexpected update expression type");
}

CompOp Analyze::convert_sv_comp_op(ast::SvCompOp op) {
    std::map<ast::SvCompOp, CompOp> m = {
        {ast::SV_OP_EQ, OP_EQ}, {ast::SV_OP_NE, OP_NE}, {ast::SV_OP_LT, OP_LT},
        {ast::SV_OP_GT, OP_GT}, {ast::SV_OP_LE, OP_LE}, {ast::SV_OP_GE, OP_GE},
    };
    return m.at(op);
}
