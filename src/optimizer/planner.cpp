/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "planner.h"
#include "explain_generator.h"
// 原型工程清单固定只编译 planner.cpp，因此在既有编译单元中纳入通用 EXPLAIN 实现。
#include "explain_generator.cpp"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "execution/executor_delete.h"
#include "execution/executor_index_scan.h"
#include "execution/executor_insert.h"
#include "execution/executor_nestedloop_join.h"
#include "execution/executor_projection.h"
#include "execution/executor_seq_scan.h"
#include "execution/executor_update.h"
#include "index/ix.h"
#include "record_printer.h"

namespace {

bool SkipScanEnabled() {
    const char *value = std::getenv("RMDB_ENABLE_INDEX_SKIP_SCAN");
    // 默认启用；显式设置为 0 可回退。
    return value == nullptr || std::string(value) != "0";
}

std::string real_table_name(const Query &query, const std::string &tab_name) {
    auto it = query.table_real_names.find(tab_name);
    return it == query.table_real_names.end() ? tab_name : it->second;
}

bool MvccSnapshotIsolation(Context *context) {
    if (context == nullptr || context->txn_ == nullptr) {
        return false;
    }
    auto isolation = context->txn_->get_isolation_level();
    return isolation == IsolationLevel::SNAPSHOT_ISOLATION || isolation == IsolationLevel::SERIALIZABLE;
}

bool MvccSafeIndexScanAvailable(Context *context) {
    // Q9-B: IndexScanExecutor merges current B+ tree candidates with StaleIndexRegistry
    // candidates, then always runs MVCC visibility and full predicate recheck.
    return MvccSnapshotIsolation(context);
}

bool DisableMvccUnsafeIndexScan(Context *context) {
    return MvccSnapshotIsolation(context) && !MvccSafeIndexScanAvailable(context);
}

bool DisableMvccUnsafeIndexJoin(Context *context) {
    // Q9-B3: IndexNestedLoopJoinExecutor uses the same current+stale candidate collection
    // as IndexScanExecutor and rechecks MVCC visibility before evaluating join predicates.
    return false;
}

// Propagate constants through equality joins. For example, `a.x = b.x AND
// b.x = 7` also gives `a.x = 7`, which can unlock a composite index on `a`.
// This is a general relational rewrite and the original predicates are kept
// for the final semantic recheck.
void PropagateEqualityConstants(
    SmManager *sm_manager, const Query &query,
    std::unordered_map<std::string, std::vector<Condition>> *filters,
    const std::vector<Condition> &join_conds) {
    auto column_meta = [&](const TabCol &column) -> const ColMeta * {
        const auto &table = sm_manager->db_.get_table(real_table_name(query, column.tab_name));
        auto it = std::find_if(table.cols.begin(), table.cols.end(), [&](const ColMeta &meta) {
            return meta.name == column.col_name;
        });
        return it == table.cols.end() ? nullptr : &*it;
    };
    auto find_constant = [&](const TabCol &column) -> const Condition * {
        auto table = filters->find(column.tab_name);
        if (table == filters->end()) return nullptr;
        for (const auto &cond : table->second) {
            if (cond.is_rhs_val && cond.op == OP_EQ && cond.lhs_col.col_name == column.col_name) {
                return &cond;
            }
        }
        return nullptr;
    };
    auto has_constant = [&](const TabCol &column) {
        return find_constant(column) != nullptr;
    };

    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto &join : join_conds) {
            if (join.is_rhs_val || join.op != OP_EQ) continue;
            const TabCol *source = nullptr;
            const TabCol *target = nullptr;
            if (has_constant(join.lhs_col) && !has_constant(join.rhs_col)) {
                source = &join.lhs_col;
                target = &join.rhs_col;
            } else if (has_constant(join.rhs_col) && !has_constant(join.lhs_col)) {
                source = &join.rhs_col;
                target = &join.lhs_col;
            }
            if (source == nullptr) continue;
            const ColMeta *source_meta = column_meta(*source);
            const ColMeta *target_meta = column_meta(*target);
            if (source_meta == nullptr || target_meta == nullptr || source_meta->type != target_meta->type ||
                source_meta->len != target_meta->len) {
                continue;
            }
            Condition derived = *find_constant(*source);
            derived.lhs_col = *target;
            (*filters)[target->tab_name].push_back(std::move(derived));
            changed = true;
        }
    }
}

}  // namespace

// 按最左前缀原则选择索引：从索引列顺序出发查找条件，避免依赖 WHERE 书写顺序
bool Planner::get_index_cols(const std::string& tab_name, const std::vector<Condition>& curr_conds,
                             std::vector<std::string>& index_col_names, const std::string &visible_tab_name) {
    index_col_names.clear();
    const std::string &cond_tab_name = visible_tab_name.empty() ? tab_name : visible_tab_name;

    TabMeta& tab = sm_manager_->db_.get_table(tab_name);
    int best_index_no = -1;
    int best_matched = 0;
    int best_eq_count = 0;
    bool best_has_range = false;

    for (size_t index_no = 0; index_no < tab.indexes.size(); ++index_no) {
        const auto& index = tab.indexes[index_no];
        int matched = 0;
        int eq_count = 0;
        bool has_range = false;

        for (const auto& index_col : index.cols) {
            bool has_eq = false;
            bool has_range_cond = false;

            // 当前索引列只接收本表字段与常量的条件，列名匹配即可自动适配 WHERE 条件顺序
            for (const auto& cond : curr_conds) {
                if (!cond.is_rhs_val || cond.lhs_col.tab_name != cond_tab_name ||
                    cond.lhs_col.col_name != index_col.name) {
                    continue;
                }
                if (cond.op == OP_EQ) {
                    has_eq = true;
                    break;  // 等值优先级高于范围，命中即可停止扫描该列剩余条件
                } else if (cond.op == OP_LT || cond.op == OP_LE || cond.op == OP_GT || cond.op == OP_GE) {
                    has_range_cond = true;
                }
            }

            if (has_eq) {
                matched++;
                eq_count++;
                continue;
            }
            if (has_range_cond) {
                matched++;
                has_range = true;
            }
            break;
        }

        if (matched > best_matched ||
            (matched == best_matched && eq_count > best_eq_count) ||
            (matched == best_matched && eq_count == best_eq_count && has_range && !best_has_range)) {
            best_index_no = static_cast<int>(index_no);
            best_matched = matched;
            best_eq_count = eq_count;
            best_has_range = has_range;
        }
    }

    // best 仅在 matched > 0 时更新，best_index_no < 0 即表示没有任何索引可用
    if (best_index_no < 0) {
        return false;
    }

    // 返回选中索引的完整列名，执行器再根据条件决定实际使用的前缀和范围
    for (const auto& col : tab.indexes[best_index_no].cols) {
        index_col_names.push_back(col.name);
    }
    return true;
}

bool Planner::get_skip_scan_index_cols(const std::string &tab_name, const std::vector<Condition> &curr_conds,
                                       std::vector<std::string> &index_col_names,
                                       const std::string &visible_tab_name) {
    index_col_names.clear();
    const std::string &cond_tab_name = visible_tab_name.empty() ? tab_name : visible_tab_name;
    TabMeta &tab = sm_manager_->db_.get_table(tab_name);
    int best_index_no = -1;
    int best_suffix_cols = 0;
    int best_eq_cols = 0;

    auto match_col = [&](const ColMeta &index_col, bool *has_eq, bool *has_range) {
        *has_eq = false;
        *has_range = false;
        for (const auto &cond : curr_conds) {
            if (!cond.is_rhs_val || cond.lhs_col.tab_name != cond_tab_name ||
                cond.lhs_col.col_name != index_col.name) {
                continue;
            }
            if (cond.op == OP_EQ) {
                *has_eq = true;
                return;
            }
            if (cond.op == OP_LT || cond.op == OP_LE || cond.op == OP_GT || cond.op == OP_GE) {
                *has_range = true;
            }
        }
    };

    for (size_t index_no = 0; index_no < tab.indexes.size(); ++index_no) {
        const auto &index = tab.indexes[index_no];
        if (index.cols.size() < 2) {
            continue;
        }
        bool leading_eq = false;
        bool leading_range = false;
        match_col(index.cols.front(), &leading_eq, &leading_range);
        if (leading_eq || leading_range) {
            continue;  // 能命中最左列时应由普通 IndexScan 处理。
        }

        int suffix_cols = 0;
        int eq_cols = 0;
        for (size_t col_idx = 1; col_idx < index.cols.size(); ++col_idx) {
            bool has_eq = false;
            bool has_range = false;
            match_col(index.cols[col_idx], &has_eq, &has_range);
            if (has_eq) {
                suffix_cols++;
                eq_cols++;
                continue;
            }
            if (has_range) {
                suffix_cols++;
            }
            break;
        }
        if (suffix_cols > best_suffix_cols ||
            (suffix_cols == best_suffix_cols && eq_cols > best_eq_cols)) {
            best_index_no = static_cast<int>(index_no);
            best_suffix_cols = suffix_cols;
            best_eq_cols = eq_cols;
        }
    }

    if (best_index_no < 0 || best_suffix_cols == 0) {
        return false;
    }
    for (const auto &col : tab.indexes[best_index_no].cols) {
        index_col_names.push_back(col.name);
    }
    return true;
}

bool Planner::is_unique_point_predicate(const std::string &tab_name, const std::vector<Condition> &conds) {
    TabMeta &tab = sm_manager_->db_.get_table(tab_name);
    for (const auto &index : tab.indexes) {
        if (index.cols.empty()) {
            continue;
        }
        bool all_eq = true;
        for (const auto &index_col : index.cols) {
            bool found = false;
            for (const auto &cond : conds) {
                bool same_table = cond.lhs_col.tab_name.empty() || cond.lhs_col.tab_name == tab_name;
                if (same_table && cond.is_rhs_val && cond.op == OP_EQ && cond.lhs_col.col_name == index_col.name) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                all_eq = false;
                break;
            }
        }
        if (all_eq) {
            return true;
        }
    }
    return false;
}

/**
 * @brief 表算子条件谓词生成
 *
 * @param conds 条件
 * @param tab_names 表名
 * @return std::vector<Condition>
 */
std::vector<Condition> pop_conds(std::vector<Condition> &conds, std::string tab_names) {
    // auto has_tab = [&](const std::string &tab_name) {
    //     return std::find(tab_names.begin(), tab_names.end(), tab_name) != tab_names.end();
    // };
    std::vector<Condition> solved_conds;
    auto it = conds.begin();
    while (it != conds.end()) {
        if ((tab_names.compare(it->lhs_col.tab_name) == 0 && it->is_rhs_val) || (it->lhs_col.tab_name.compare(it->rhs_col.tab_name) == 0)) {
            solved_conds.emplace_back(std::move(*it));
            it = conds.erase(it);
        } else {
            it++;
        }
    }
    return solved_conds;
}

int push_conds(Condition *cond, std::shared_ptr<Plan> plan)
{
    if(auto x = std::dynamic_pointer_cast<ScanPlan>(plan))
    {
        if(x->tab_name_.compare(cond->lhs_col.tab_name) == 0) {
            return 1;
        } else if(x->tab_name_.compare(cond->rhs_col.tab_name) == 0){
            return 2;
        } else {
            return 0;
        }
    }
    else if(auto x = std::dynamic_pointer_cast<JoinPlan>(plan))
    {
        int left_res = push_conds(cond, x->left_);
        // 条件已经下推到左子节点
        if(left_res == 3){
            return 3;
        }
        int right_res = push_conds(cond, x->right_);
        // 条件已经下推到右子节点
        if(right_res == 3){
            return 3;
        }
        // 左子节点或右子节点有一个没有匹配到条件的列
        if(left_res == 0 || right_res == 0) {
            return left_res + right_res;
        }
        // 左子节点匹配到条件的右边
        if(left_res == 2) {
            // 需要将左右两边的条件变换位置
            std::map<CompOp, CompOp> swap_op = {
                {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
            };
            std::swap(cond->lhs_col, cond->rhs_col);
            cond->op = swap_op.at(cond->op);
        }
        x->conds_.emplace_back(std::move(*cond));
        return 3;
    }
    return false;
}

std::shared_ptr<Plan> pop_scan(int *scantbl, std::string table, std::vector<std::string> &joined_tables, 
                std::vector<std::shared_ptr<Plan>> plans)
{
    for (size_t i = 0; i < plans.size(); i++) {
        auto x = std::dynamic_pointer_cast<ScanPlan>(plans[i]);
        if(x->tab_name_.compare(table) == 0)
        {
            scantbl[i] = 1;
            joined_tables.emplace_back(x->tab_name_);
            return plans[i];
        }
    }
    return nullptr;
}


std::shared_ptr<Query> Planner::logical_optimization(std::shared_ptr<Query> query, Context *context)
{
    
    //TODO 实现逻辑优化规则

    return query;
}

std::shared_ptr<Plan> Planner::physical_optimization(std::shared_ptr<Query> query, Context *context)
{
    std::shared_ptr<Plan> plan = make_left_deep_join_plan(query, context);

    return plan;
}



std::shared_ptr<Plan> Planner::make_one_rel(std::shared_ptr<Query> query)
{
    auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse);
    std::vector<std::string> tables = query->tables;
    // // Scan table , 生成表算子列表tab_nodes
    std::vector<std::shared_ptr<Plan>> table_scan_executors(tables.size());
    for (size_t i = 0; i < tables.size(); i++) {
        auto curr_conds = pop_conds(query->conds, tables[i]);
        // int index_no = get_indexNo(tables[i], curr_conds);
        std::vector<std::string> index_col_names;
        bool index_exist = get_index_cols(tables[i], curr_conds, index_col_names);
        if (index_exist == false) {  // 该表没有索引
            index_col_names.clear();
            table_scan_executors[i] = 
                std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, tables[i], curr_conds, index_col_names);
        } else {  // 存在索引
            table_scan_executors[i] =
                std::make_shared<ScanPlan>(T_IndexScan, sm_manager_, tables[i], curr_conds, index_col_names);
        }
    }
    // 只有一个表，不需要join。
    if(tables.size() == 1)
    {
        return table_scan_executors[0];
    }
    // 获取where条件
    auto conds = std::move(query->conds);
    std::shared_ptr<Plan> table_join_executors;
    
    int scantbl[tables.size()];
    for(size_t i = 0; i < tables.size(); i++)
    {
        scantbl[i] = -1;
    }
    // 假设在ast中已经添加了jointree，这里需要修改的逻辑是，先处理jointree，然后再考虑剩下的部分
    if(conds.size() >= 1)
    {
        // 有连接条件

        // 根据连接条件，生成第一层join
        std::vector<std::string> joined_tables(tables.size());
        auto it = conds.begin();
        while (it != conds.end()) {
            std::shared_ptr<Plan> left , right;
            left = pop_scan(scantbl, it->lhs_col.tab_name, joined_tables, table_scan_executors);
            right = pop_scan(scantbl, it->rhs_col.tab_name, joined_tables, table_scan_executors);
            std::vector<Condition> join_conds{*it};
            //建立join
            // 判断使用哪种join方式
            if(enable_nestedloop_join && enable_sortmerge_join) {
                // 默认nested loop join
                table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(left), std::move(right), join_conds);
            } else if(enable_nestedloop_join) {
                table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(left), std::move(right), join_conds);
            } else if(enable_sortmerge_join) {
                table_join_executors = std::make_shared<JoinPlan>(T_SortMerge, std::move(left), std::move(right), join_conds);
            } else {
                // error
                throw RMDBError("No join executor selected!");
            }

            // table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(left), std::move(right), join_conds);
            it = conds.erase(it);
            break;
        }
        // 根据连接条件，生成第2-n层join
        it = conds.begin();
        while (it != conds.end()) {
            std::shared_ptr<Plan> left_need_to_join_executors = nullptr;
            std::shared_ptr<Plan> right_need_to_join_executors = nullptr;
            bool isneedreverse = false;
            if (std::find(joined_tables.begin(), joined_tables.end(), it->lhs_col.tab_name) == joined_tables.end()) {
                left_need_to_join_executors = pop_scan(scantbl, it->lhs_col.tab_name, joined_tables, table_scan_executors);
            }
            if (std::find(joined_tables.begin(), joined_tables.end(), it->rhs_col.tab_name) == joined_tables.end()) {
                right_need_to_join_executors = pop_scan(scantbl, it->rhs_col.tab_name, joined_tables, table_scan_executors);
                isneedreverse = true;
            } 

            if(left_need_to_join_executors != nullptr && right_need_to_join_executors != nullptr) {
                std::vector<Condition> join_conds{*it};
                std::shared_ptr<Plan> temp_join_executors = std::make_shared<JoinPlan>(T_NestLoop, 
                                                                    std::move(left_need_to_join_executors), 
                                                                    std::move(right_need_to_join_executors), 
                                                                    join_conds);
                table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(temp_join_executors), 
                                                                    std::move(table_join_executors), 
                                                                    std::vector<Condition>());
            } else if(left_need_to_join_executors != nullptr || right_need_to_join_executors != nullptr) {
                if(isneedreverse) {
                    std::map<CompOp, CompOp> swap_op = {
                        {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
                    };
                    std::swap(it->lhs_col, it->rhs_col);
                    it->op = swap_op.at(it->op);
                    left_need_to_join_executors = std::move(right_need_to_join_executors);
                }
                std::vector<Condition> join_conds{*it};
                table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(left_need_to_join_executors), 
                                                                    std::move(table_join_executors), join_conds);
            } else {
                push_conds(std::move(&(*it)), table_join_executors);
            }
            it = conds.erase(it);
        }
    } else {
        table_join_executors = table_scan_executors[0];
        scantbl[0] = 1;
    }

    //连接剩余表
    for (size_t i = 0; i < tables.size(); i++) {
        if(scantbl[i] == -1) {
            table_join_executors = std::make_shared<JoinPlan>(T_NestLoop, std::move(table_scan_executors[i]), 
                                                    std::move(table_join_executors), std::vector<Condition>());
        }
    }

    return table_join_executors;

}


std::shared_ptr<Plan> Planner::make_left_deep_join_plan(const std::shared_ptr<Query> &query, Context *context) {
    if (query->tables.empty()) throw RMDBError("SELECT requires at least one table");
    bool disable_index_scan = DisableMvccUnsafeIndexScan(context);
    bool disable_index_join = DisableMvccUnsafeIndexJoin(context);

    std::unordered_map<std::string, std::vector<Condition>> filters;
    std::vector<Condition> join_conds = query->join_conds;
    for (const auto &cond : query->where_conds) {
        if (cond.is_rhs_val || cond.rhs_col.tab_name == cond.lhs_col.tab_name) {
            filters[cond.lhs_col.tab_name].push_back(cond);
        } else {
            join_conds.push_back(cond);
        }
    }
    PropagateEqualityConstants(sm_manager_, *query, &filters, join_conds);

    const bool scalar_aggregate = query->tables.size() == 1 && query->has_aggregation && query->group_cols.empty() &&
        std::all_of(query->select_exprs.begin(), query->select_exprs.end(),
                    [](const SelectExpr &expr) { return expr.is_aggregate; });

    auto make_scan = [&](const std::string &table) -> std::shared_ptr<Plan> {
        std::string real_table = real_table_name(*query, table);
        std::vector<std::string> index_cols;
        bool use_index = !disable_index_scan && get_index_cols(real_table, filters[table], index_cols, table);
        bool use_skip_scan = false;
        // 仅改写无 GROUP BY 的标量聚合，避免改变普通 SELECT 或分组结果的既有行序。
        if (!use_index && !disable_index_scan && scalar_aggregate && SkipScanEnabled()) {
            use_skip_scan = get_skip_scan_index_cols(real_table, filters[table], index_cols, table);
        }
        if (!use_index && !use_skip_scan) {
            index_cols.clear();
        }
        return std::make_shared<ScanPlan>(use_index || use_skip_scan ? T_IndexScan : T_SeqScan, sm_manager_, table,
                                          filters[table], std::move(index_cols), real_table, use_skip_scan);
    };

    std::shared_ptr<Plan> root = make_scan(query->tables.front());
    std::unordered_set<std::string> joined{query->tables.front()};

    for (size_t table_idx = 1; table_idx < query->tables.size(); ++table_idx) {
        const std::string &right_table = query->tables[table_idx];
        std::vector<Condition> level_conds;
        for (const auto &cond : join_conds) {
            if (cond.is_rhs_val) continue;
            bool lhs_right = cond.lhs_col.tab_name == right_table;
            bool rhs_right = cond.rhs_col.tab_name == right_table;
            bool connects_joined = (lhs_right && joined.count(cond.rhs_col.tab_name) > 0) ||
                                   (rhs_right && joined.count(cond.lhs_col.tab_name) > 0);
            if (connects_joined) level_conds.push_back(cond);
        }

        bool use_inlj = false;
        TabCol outer_key;
        TabCol inner_key;
        const std::string right_real_table = real_table_name(*query, right_table);
        const auto &right_meta = sm_manager_->db_.get_table(right_real_table);
        for (const auto &cond : level_conds) {
            if (cond.op != OP_EQ || cond.is_rhs_val) continue;
            if (!disable_index_join && cond.lhs_col.tab_name == right_table &&
                joined.count(cond.rhs_col.tab_name) > 0 &&
                right_meta.is_index({cond.lhs_col.col_name})) {
                inner_key = cond.lhs_col;
                outer_key = cond.rhs_col;
                use_inlj = true;
                break;
            }
            if (!disable_index_join && cond.rhs_col.tab_name == right_table &&
                joined.count(cond.lhs_col.tab_name) > 0 &&
                right_meta.is_index({cond.rhs_col.col_name})) {
                inner_key = cond.rhs_col;
                outer_key = cond.lhs_col;
                use_inlj = true;
                break;
            }
        }

        if (use_inlj) {
            root = std::make_shared<IndexNestedLoopJoinPlan>(root, right_table, right_real_table,
                                                             filters[right_table], level_conds, outer_key, inner_key);
        } else {
            root = std::make_shared<JoinPlan>(T_NestLoop, root, make_scan(right_table), level_conds);
        }
        joined.insert(right_table);
    }
    return root;
}

std::shared_ptr<Plan> Planner::generate_sort_plan(std::shared_ptr<Query> query, std::shared_ptr<Plan> plan)
{
    if (query->order_specs.empty() && query->limit < 0) {
        return plan;
    }
    std::vector<TabCol> sort_cols;
    std::vector<bool> directions;
    for (const auto &order : query->order_specs) {
        if (query->has_aggregation) {
            auto match = std::find_if(query->select_exprs.begin(), query->select_exprs.end(),
                [&](const SelectExpr &expr) {
                    return expr.output_name == order.expr.output_name ||
                           (expr.is_aggregate == order.expr.is_aggregate && expr.agg_func == order.expr.agg_func &&
                            expr.count_star == order.expr.count_star && expr.col.tab_name == order.expr.col.tab_name &&
                            expr.col.col_name == order.expr.col.col_name);
                });
            if (match == query->select_exprs.end()) {
                throw RMDBError("ORDER BY expression must appear in aggregate SELECT list");
            }
            sort_cols.push_back({"", match->output_name});
        } else {
            sort_cols.push_back(order.expr.col);
        }
        directions.push_back(order.is_desc);
    }
    return std::make_shared<SortPlan>(T_Sort, std::move(plan), std::move(sort_cols),
                                      std::move(directions), query->limit);
}


/**
 * @brief select plan 生成
 *
 * @param sel_cols select plan 选取的列
 * @param tab_names select plan 目标的表
 * @param conds select plan 选取条件
 */
std::shared_ptr<Plan> Planner::generate_select_plan(std::shared_ptr<Query> query, Context *context) {
    //逻辑优化
    query = logical_optimization(std::move(query), context);

    //物理优化
    std::shared_ptr<Plan> plannerRoot;
    if (query->has_union_source) {
        std::vector<std::shared_ptr<Plan>> branch_plans;
        branch_plans.reserve(query->union_branches.size());
        for (auto &branch : query->union_branches) {
            branch_plans.push_back(generate_select_plan(branch, context));
        }
        plannerRoot = std::make_shared<UnionPlan>(std::move(branch_plans), query->output_cols);
    } else {
        plannerRoot = physical_optimization(query, context);
    }
    if (query->has_aggregation) {
        plannerRoot = std::make_shared<AggregatePlan>(std::move(plannerRoot), query->select_exprs,
                                                       query->group_cols, query->having_conds);
        plannerRoot = generate_sort_plan(query, std::move(plannerRoot));
    } else {
        plannerRoot = generate_sort_plan(query, std::move(plannerRoot));
        plannerRoot = std::make_shared<ProjectionPlan>(T_Projection, std::move(plannerRoot), query->cols);
    }

    return plannerRoot;
}

// 生成DDL语句和DML语句的查询执行计划
std::shared_ptr<Plan> Planner::do_planner(std::shared_ptr<Query> query, Context *context)
{
    std::shared_ptr<Plan> plannerRoot;
    if (auto explain = std::dynamic_pointer_cast<ast::ExplainAnalyzeStmt>(query->parse)) {
        plannerRoot = std::make_shared<ExplainPlan>(make_rmdb_explain_lines(explain->select, sm_manager_));
    } else if (auto x = std::dynamic_pointer_cast<ast::CreateTable>(query->parse)) {
        // create table;
        std::vector<ColDef> col_defs;
        for (auto &field : x->fields) {
            if (auto sv_col_def = std::dynamic_pointer_cast<ast::ColDef>(field)) {
                ColDef col_def = {.name = sv_col_def->col_name,
                                  .type = interp_sv_type(sv_col_def->type_len->type),
                                  .len = sv_col_def->type_len->len};
                col_defs.push_back(col_def);
            } else {
                throw InternalError("Unexpected field type");
            }
        }
        plannerRoot = std::make_shared<DDLPlan>(T_CreateTable, x->tab_name, std::vector<std::string>(), col_defs);
    } else if (auto x = std::dynamic_pointer_cast<ast::DropTable>(query->parse)) {
        // drop table;
        plannerRoot = std::make_shared<DDLPlan>(T_DropTable, x->tab_name, std::vector<std::string>(), std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::CreateIndex>(query->parse)) {
        // create index;
        plannerRoot = std::make_shared<DDLPlan>(T_CreateIndex, x->tab_name, x->col_names, std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::DropIndex>(query->parse)) {
        // drop index
        plannerRoot = std::make_shared<DDLPlan>(T_DropIndex, x->tab_name, x->col_names, std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(query->parse)) {
        // insert;
        plannerRoot = std::make_shared<DMLPlan>(T_Insert, std::shared_ptr<Plan>(),  x->tab_name,  
                                                    query->values, std::vector<Condition>(), std::vector<SetClause>());
    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(query->parse)) {
        // delete;
        // 生成表扫描方式
        std::shared_ptr<Plan> table_scan_executors;
        // 只有一张表，不需要进行物理优化了
        // int index_no = get_indexNo(x->tab_name, query->conds);
        std::vector<std::string> index_col_names;
        bool index_exist = !DisableMvccUnsafeIndexScan(context) &&
                           get_index_cols(x->tab_name, query->conds, index_col_names);

        if (index_exist == false) {  // 该表没有索引
            index_col_names.clear();
            table_scan_executors = 
                std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        } else {  // 存在索引
            table_scan_executors =
                std::make_shared<ScanPlan>(T_IndexScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        }

        auto delete_plan = std::make_shared<DMLPlan>(T_Delete, table_scan_executors, x->tab_name,
                                                     std::vector<Value>(), query->conds, std::vector<SetClause>());
        delete_plan->expect_existing_unique_point_ = is_unique_point_predicate(x->tab_name, query->conds);
        plannerRoot = delete_plan;
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(query->parse)) {
        // update;
        // 生成表扫描方式
        std::shared_ptr<Plan> table_scan_executors;
        // 只有一张表，不需要进行物理优化了
        // int index_no = get_indexNo(x->tab_name, query->conds);
        std::vector<std::string> index_col_names;
        bool index_exist = !DisableMvccUnsafeIndexScan(context) &&
                           get_index_cols(x->tab_name, query->conds, index_col_names);

        if (index_exist == false) {  // 该表没有索引
            index_col_names.clear();
            table_scan_executors =
                std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        } else {  // 存在索引
            table_scan_executors =
                std::make_shared<ScanPlan>(T_IndexScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        }
        auto update_plan = std::make_shared<DMLPlan>(T_Update, table_scan_executors, x->tab_name,
                                                     std::vector<Value>(), query->conds,
                                                     query->set_clauses);
        update_plan->expect_existing_unique_point_ = is_unique_point_predicate(x->tab_name, query->conds);
        plannerRoot = update_plan;
    } else if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse)) {

        std::shared_ptr<plannerInfo> root = std::make_shared<plannerInfo>(x);
        // 生成select语句的查询执行计划
        std::shared_ptr<Plan> projection = generate_select_plan(query, context);
        auto select_plan = std::make_shared<DMLPlan>(T_select, projection, std::string(), std::vector<Value>(),
                                                     std::vector<Condition>(), std::vector<SetClause>());
        for (const auto &expr : query->select_exprs) {
            select_plan->result_cols_.push_back({"", expr.output_name});
        }
        plannerRoot = std::move(select_plan);
    } else {
        throw InternalError("Unexpected AST root");
    }
    return plannerRoot;
}
