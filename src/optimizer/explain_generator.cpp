#include "explain_generator.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <utility>

#include "record/rm_scan.h"

namespace {

std::string col_to_string(const std::shared_ptr<ast::Col> &col) {
    return col->tab_name.empty() ? col->col_name : col->tab_name + "." + col->col_name;
}

std::string value_to_string(const std::shared_ptr<ast::Value> &value) {
    if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(value)) {
        return std::to_string(int_lit->val);
    }
    if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(value)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.6f", float_lit->val);
        return buf;
    }
    if (auto str_lit = std::dynamic_pointer_cast<ast::StringLit>(value)) {
        return "'" + str_lit->val + "'";
    }
    return "";
}

std::string op_to_string(ast::SvCompOp op) {
    switch (op) {
        case ast::SV_OP_EQ: return "=";
        case ast::SV_OP_NE: return "<>";
        case ast::SV_OP_LT: return "<";
        case ast::SV_OP_GT: return ">";
        case ast::SV_OP_LE: return "<=";
        case ast::SV_OP_GE: return ">=";
    }
    return "";
}

std::string cond_to_string(const std::shared_ptr<ast::BinaryExpr> &cond) {
    std::string rhs;
    if (auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs)) {
        rhs = col_to_string(rhs_col);
    } else if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(cond->rhs)) {
        rhs = value_to_string(rhs_val);
    }
    return col_to_string(cond->lhs) + op_to_string(cond->op) + rhs;
}

std::string join_strings(std::vector<std::string> items) {
    std::sort(items.begin(), items.end());
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i != 0) out += ", ";
        out += items[i];
    }
    return out;
}

std::string join_tables_string(const std::set<std::string> &tables) {
    return join_strings(std::vector<std::string>(tables.begin(), tables.end()));
}

bool table_has_filter(const std::vector<std::shared_ptr<ast::BinaryExpr>> &conds,
                      const std::string &visible) {
    for (const auto &cond : conds) {
        auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
        if (cond->lhs->tab_name == visible &&
            (std::dynamic_pointer_cast<ast::Value>(cond->rhs) ||
             (rhs_col && rhs_col->tab_name == visible))) {
            return true;
        }
    }
    return false;
}

std::string ast_col_key(const std::shared_ptr<ast::Col> &col) {
    return col->tab_name + "\x1f" + col->col_name;
}

void propagate_equal_join_value_filters(std::vector<std::shared_ptr<ast::BinaryExpr>> &conds) {
    std::map<std::string, std::shared_ptr<ast::Col>> key_to_col;
    std::map<std::string, std::vector<std::string>> graph;
    for (const auto &cond : conds) {
        auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
        if (!rhs_col || cond->op != ast::SV_OP_EQ || cond->lhs->tab_name == rhs_col->tab_name) {
            continue;
        }
        std::string lhs_key = ast_col_key(cond->lhs);
        std::string rhs_key = ast_col_key(rhs_col);
        key_to_col[lhs_key] = cond->lhs;
        key_to_col[rhs_key] = rhs_col;
        graph[lhs_key].push_back(rhs_key);
        graph[rhs_key].push_back(lhs_key);
    }

    std::map<std::string, std::vector<std::shared_ptr<ast::Col>>> component_cols;
    std::set<std::string> visited;
    for (const auto &entry : key_to_col) {
        if (!visited.insert(entry.first).second) continue;
        std::vector<std::string> stack{entry.first};
        std::vector<std::shared_ptr<ast::Col>> cols;
        while (!stack.empty()) {
            std::string current = stack.back();
            stack.pop_back();
            cols.push_back(key_to_col.at(current));
            for (const auto &next : graph[current]) {
                if (visited.insert(next).second) stack.push_back(next);
            }
        }
        for (const auto &col : cols) component_cols[ast_col_key(col)] = cols;
    }

    std::set<std::string> existing;
    for (const auto &cond : conds) existing.insert(cond_to_string(cond));
    std::vector<std::shared_ptr<ast::BinaryExpr>> additions;
    for (const auto &cond : conds) {
        if (!std::dynamic_pointer_cast<ast::Value>(cond->rhs)) continue;
        auto component = component_cols.find(ast_col_key(cond->lhs));
        if (component == component_cols.end()) continue;
        for (const auto &target_col : component->second) {
            if (target_col->tab_name == cond->lhs->tab_name &&
                target_col->col_name == cond->lhs->col_name) {
                continue;
            }
            auto lhs = std::make_shared<ast::Col>(target_col->tab_name, target_col->col_name);
            auto derived = std::make_shared<ast::BinaryExpr>(lhs, cond->op, cond->rhs);
            if (existing.insert(cond_to_string(derived)).second) additions.push_back(std::move(derived));
        }
    }
    conds.insert(conds.end(), additions.begin(), additions.end());
}

std::string table_filter_string(const std::vector<std::shared_ptr<ast::BinaryExpr>> &conds,
                                const std::string &visible) {
    std::vector<std::string> filters;
    for (const auto &cond : conds) {
        auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
        if (cond->lhs->tab_name == visible &&
            (std::dynamic_pointer_cast<ast::Value>(cond->rhs) ||
             (rhs_col && rhs_col->tab_name == visible))) {
            filters.push_back(cond_to_string(cond));
        }
    }
    return join_strings(std::move(filters));
}

}  // namespace

std::vector<std::string> make_rmdb_explain_lines(const std::shared_ptr<ast::SelectStmt> &select,
                                                 SmManager *sm_manager) {
    std::map<std::string, std::string> alias_to_table;
    std::vector<std::string> input_visible;
    for (const auto &ref : select->table_refs) {
        alias_to_table[ref->visible_name()] = ref->tab_name;
        input_visible.push_back(ref->visible_name());
    }

    if (input_visible.size() == 1) {
        const std::string &default_alias = input_visible.front();
        for (auto &col : select->cols) {
            if (col && col->tab_name.empty()) col->tab_name = default_alias;
        }
        for (auto &cond : select->conds) {
            if (cond->lhs->tab_name.empty()) cond->lhs->tab_name = default_alias;
            if (auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs)) {
                if (rhs_col->tab_name.empty()) rhs_col->tab_name = default_alias;
            }
        }
    }

    auto explain_conds = select->conds;
    propagate_equal_join_value_filters(explain_conds);

    auto table_rows = [&](const std::string &alias) {
        int rows = 0;
        auto fh = sm_manager->fhs_.at(alias_to_table.at(alias)).get();
        for (RmScan scan(fh); !scan.is_end(); scan.next()) ++rows;
        return rows;
    };

    auto table_has_join_index = [&](const std::string &alias,
                                    const std::shared_ptr<ast::BinaryExpr> &cond) {
        std::vector<std::string> cols;
        auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
        if (cond->lhs->tab_name == alias) cols.push_back(cond->lhs->col_name);
        else if (rhs_col && rhs_col->tab_name == alias) cols.push_back(rhs_col->col_name);
        if (cols.empty()) return false;
        return sm_manager->db_.get_table(alias_to_table.at(alias)).is_index(cols);
    };

    auto read_col = [&](const std::string &alias, const Rid &rid, const std::string &col_name) {
        const std::string &table_name = alias_to_table.at(alias);
        auto &table = sm_manager->db_.get_table(table_name);
        auto col = table.get_col(col_name);
        auto record = sm_manager->fhs_.at(table_name)->get_record(rid, nullptr);
        std::string raw(record->data + col->offset, col->len);
        return std::pair<ColMeta, std::string>(*col, std::move(raw));
    };

    auto compare_raw = [&](const ColMeta &lhs_col, const std::string &lhs_raw,
                           const ColMeta &rhs_col, const std::string &rhs_raw,
                           ast::SvCompOp op) {
        int cmp = 0;
        if (lhs_col.type == TYPE_FLOAT || rhs_col.type == TYPE_FLOAT) {
            float lhs = lhs_col.type == TYPE_FLOAT ? *reinterpret_cast<const float *>(lhs_raw.data())
                                                   : static_cast<float>(*reinterpret_cast<const int *>(lhs_raw.data()));
            float rhs = rhs_col.type == TYPE_FLOAT ? *reinterpret_cast<const float *>(rhs_raw.data())
                                                   : static_cast<float>(*reinterpret_cast<const int *>(rhs_raw.data()));
            cmp = (lhs > rhs) - (lhs < rhs);
        } else if (lhs_col.type == TYPE_INT) {
            int lhs = *reinterpret_cast<const int *>(lhs_raw.data());
            int rhs = *reinterpret_cast<const int *>(rhs_raw.data());
            cmp = (lhs > rhs) - (lhs < rhs);
        } else {
            std::string lhs = lhs_raw;
            std::string rhs = rhs_raw;
            lhs.resize(strlen(lhs.c_str()));
            rhs.resize(strlen(rhs.c_str()));
            cmp = lhs.compare(rhs);
        }
        switch (op) {
            case ast::SV_OP_EQ: return cmp == 0;
            case ast::SV_OP_NE: return cmp != 0;
            case ast::SV_OP_LT: return cmp < 0;
            case ast::SV_OP_GT: return cmp > 0;
            case ast::SV_OP_LE: return cmp <= 0;
            case ast::SV_OP_GE: return cmp >= 0;
        }
        return false;
    };

    auto compare_value_cond = [&](const std::string &alias, const Rid &rid,
                                  const std::shared_ptr<ast::BinaryExpr> &cond) {
        auto [lhs_col, lhs_raw] = read_col(alias, rid, cond->lhs->col_name);
        ColMeta rhs_col = lhs_col;
        std::string rhs_raw(lhs_col.len, '\0');
        if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(cond->rhs)) {
            int value = int_lit->val;
            if (lhs_col.type == TYPE_FLOAT) {
                float converted = static_cast<float>(value);
                memcpy(rhs_raw.data(), &converted, sizeof(float));
                rhs_col.type = TYPE_FLOAT;
                rhs_col.len = sizeof(float);
            } else {
                memcpy(rhs_raw.data(), &value, sizeof(int));
                rhs_col.type = TYPE_INT;
                rhs_col.len = sizeof(int);
            }
        } else if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(cond->rhs)) {
            float value = float_lit->val;
            memcpy(rhs_raw.data(), &value, sizeof(float));
            rhs_col.type = TYPE_FLOAT;
            rhs_col.len = sizeof(float);
        } else if (auto str_lit = std::dynamic_pointer_cast<ast::StringLit>(cond->rhs)) {
            rhs_raw.assign(lhs_col.len, '\0');
            memcpy(rhs_raw.data(), str_lit->val.c_str(), std::min<int>(lhs_col.len, str_lit->val.size()));
            rhs_col.type = TYPE_STRING;
        }
        return compare_raw(lhs_col, lhs_raw, rhs_col, rhs_raw, cond->op);
    };

    std::map<std::string, std::vector<std::shared_ptr<ast::BinaryExpr>>> local_conds;
    std::vector<std::shared_ptr<ast::BinaryExpr>> join_conds;
    for (const auto &cond : explain_conds) {
        auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
        if (rhs_col && rhs_col->tab_name != cond->lhs->tab_name) join_conds.push_back(cond);
        else local_conds[cond->lhs->tab_name].push_back(cond);
    }

    auto compare_filter_cond = [&](const std::string &alias, const Rid &rid,
                                   const std::shared_ptr<ast::BinaryExpr> &cond) {
        if (std::dynamic_pointer_cast<ast::Value>(cond->rhs)) return compare_value_cond(alias, rid, cond);
        auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
        auto [lhs_meta, lhs_raw] = read_col(alias, rid, cond->lhs->col_name);
        auto [rhs_meta, rhs_raw] = read_col(alias, rid, rhs_col->col_name);
        return compare_raw(lhs_meta, lhs_raw, rhs_meta, rhs_raw, cond->op);
    };

    auto filtered_rows = [&](const std::string &alias) {
        int rows = 0;
        auto fh = sm_manager->fhs_.at(alias_to_table.at(alias)).get();
        for (RmScan scan(fh); !scan.is_end(); scan.next()) {
            bool pass = true;
            for (const auto &cond : local_conds[alias]) {
                if (!compare_filter_cond(alias, scan.rid(), cond)) {
                    pass = false;
                    break;
                }
            }
            if (pass) ++rows;
        }
        return rows;
    };

    struct EnumerationResult {
        int count = 0;
        std::vector<std::map<std::string, Rid>> records;
    };
    auto enumerate = [&](const std::set<std::string> &aliases, bool keep_records) {
        std::vector<std::string> order;
        for (const auto &alias : input_visible) {
            if (aliases.count(alias)) order.push_back(alias);
        }
        EnumerationResult result;
        std::map<std::string, Rid> current;
        std::function<void(size_t)> dfs = [&](size_t index) {
            if (index == order.size()) {
                for (const auto &cond : explain_conds) {
                    auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
                    if (rhs_col) {
                        if (!aliases.count(cond->lhs->tab_name) || !aliases.count(rhs_col->tab_name)) continue;
                        auto [lhs_meta, lhs_raw] = read_col(cond->lhs->tab_name, current[cond->lhs->tab_name],
                                                           cond->lhs->col_name);
                        auto [rhs_meta, rhs_raw] = read_col(rhs_col->tab_name, current[rhs_col->tab_name],
                                                           rhs_col->col_name);
                        if (!compare_raw(lhs_meta, lhs_raw, rhs_meta, rhs_raw, cond->op)) return;
                    } else if (aliases.count(cond->lhs->tab_name) &&
                               !compare_filter_cond(cond->lhs->tab_name, current[cond->lhs->tab_name], cond)) {
                        return;
                    }
                }
                ++result.count;
                if (keep_records) result.records.push_back(current);
                return;
            }
            const std::string &alias = order[index];
            auto fh = sm_manager->fhs_.at(alias_to_table.at(alias)).get();
            for (RmScan scan(fh); !scan.is_end(); scan.next()) {
                current[alias] = scan.rid();
                dfs(index + 1);
            }
        };
        dfs(0);
        return result;
    };

    auto join_count = [&](const std::set<std::string> &aliases) {
        return enumerate(aliases, false).count;
    };

    auto find_index_cond = [&](const std::string &right_alias,
                               const std::vector<std::shared_ptr<ast::BinaryExpr>> &conds)
        -> std::shared_ptr<ast::BinaryExpr> {
        for (const auto &cond : conds) {
            if (cond->op == ast::SV_OP_EQ && table_has_join_index(right_alias, cond)) return cond;
        }
        return nullptr;
    };

    auto index_hit_count = [&](const std::set<std::string> &outer_aliases, const std::string &right_alias,
                               const std::shared_ptr<ast::BinaryExpr> &index_cond) {
        if (!index_cond) return 0;
        auto rhs_col = std::dynamic_pointer_cast<ast::Col>(index_cond->rhs);
        std::shared_ptr<ast::Col> right_col;
        std::shared_ptr<ast::Col> outer_col;
        if (index_cond->lhs->tab_name == right_alias && rhs_col && outer_aliases.count(rhs_col->tab_name)) {
            right_col = index_cond->lhs;
            outer_col = rhs_col;
        } else if (rhs_col && rhs_col->tab_name == right_alias && outer_aliases.count(index_cond->lhs->tab_name)) {
            right_col = rhs_col;
            outer_col = index_cond->lhs;
        } else {
            return 0;
        }

        int rows = 0;
        auto prefixes = enumerate(outer_aliases, true).records;
        auto right_fh = sm_manager->fhs_.at(alias_to_table.at(right_alias)).get();
        for (const auto &prefix : prefixes) {
            auto [outer_meta, outer_raw] = read_col(outer_col->tab_name, prefix.at(outer_col->tab_name),
                                                    outer_col->col_name);
            for (RmScan scan(right_fh); !scan.is_end(); scan.next()) {
                auto [right_meta, right_raw] = read_col(right_alias, scan.rid(), right_col->col_name);
                if (!compare_raw(right_meta, right_raw, outer_meta, outer_raw, ast::SV_OP_EQ)) continue;
                bool pass = true;
                for (const auto &cond : local_conds[right_alias]) {
                    if (!compare_filter_cond(right_alias, scan.rid(), cond)) {
                        pass = false;
                        break;
                    }
                }
                if (pass) ++rows;
            }
        }
        return rows;
    };

    std::vector<std::string> lines;
    std::set<std::string> all_aliases(input_visible.begin(), input_visible.end());
    int final_rows = input_visible.empty() ? 0 : join_count(all_aliases);
    int project_rows = select->limit >= 0 ? std::min(final_rows, select->limit) : final_rows;
    if (select->cols.empty()) {
        lines.push_back("Project(columns=[*], rows=" + std::to_string(project_rows) + ")");
    } else {
        std::vector<std::string> columns;
        for (const auto &col : select->cols) {
            if (col && !col->is_aggregate) columns.push_back(col_to_string(col));
        }
        lines.push_back("Project(columns=[" + join_strings(std::move(columns)) + "], rows=" +
                        std::to_string(project_rows) + ")");
    }

    std::set<std::string> joined_aliases;
    std::map<std::string, int> leaf_multiplier;
    std::map<std::string, int> leaf_output_rows;
    std::map<std::string, std::shared_ptr<ast::BinaryExpr>> leaf_index_cond;
    std::map<std::string, bool> leaf_index;
    std::map<std::string, int> leaf_depth;
    std::vector<std::string> leaf_order;
    std::vector<std::string> join_lines;

    if (!join_conds.empty()) {
        struct JoinStep {
            std::set<std::string> aliases;
            std::vector<std::shared_ptr<ast::BinaryExpr>> conds;
        };
        std::vector<JoinStep> join_steps;
        int table_count = static_cast<int>(input_visible.size());
        if (!input_visible.empty()) {
            const std::string &first_alias = input_visible.front();
            leaf_multiplier[first_alias] = 1;
            leaf_output_rows[first_alias] = filtered_rows(first_alias);
            leaf_index[first_alias] = false;
            leaf_depth[first_alias] = table_count;
            leaf_order.push_back(first_alias);
            joined_aliases.insert(first_alias);
        }

        for (size_t i = 1; i < input_visible.size(); ++i) {
            const std::string &right_alias = input_visible[i];
            std::vector<std::shared_ptr<ast::BinaryExpr>> step_conds;
            for (const auto &cond : join_conds) {
                auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
                if (!rhs_col) continue;
                bool lhs_right = cond->lhs->tab_name == right_alias && joined_aliases.count(rhs_col->tab_name);
                bool rhs_right = rhs_col->tab_name == right_alias && joined_aliases.count(cond->lhs->tab_name);
                if (lhs_right || rhs_right) step_conds.push_back(cond);
            }
            int prefix_rows = join_count(joined_aliases);
            leaf_multiplier[right_alias] = prefix_rows;
            leaf_index_cond[right_alias] = find_index_cond(right_alias, step_conds);
            leaf_index[right_alias] = leaf_index_cond[right_alias] != nullptr;
            leaf_depth[right_alias] = table_count - static_cast<int>(i) + 1;
            leaf_output_rows[right_alias] = leaf_index[right_alias]
                ? index_hit_count(joined_aliases, right_alias, leaf_index_cond[right_alias])
                : filtered_rows(right_alias) * prefix_rows;
            leaf_order.push_back(right_alias);
            joined_aliases.insert(right_alias);
            if (!step_conds.empty()) join_steps.push_back({joined_aliases, std::move(step_conds)});
        }

        for (const auto &step : join_steps) {
            std::set<std::string> tables;
            for (const auto &alias : step.aliases) tables.insert(alias_to_table.at(alias));
            std::vector<std::string> conditions;
            for (const auto &cond : step.conds) conditions.push_back(cond_to_string(cond));
            join_lines.push_back("Join(tables=[" + join_tables_string(tables) + "], condition=[" +
                                 join_strings(std::move(conditions)) + "], rows=" +
                                 std::to_string(join_count(step.aliases)) + ")");
        }
        for (int i = static_cast<int>(join_lines.size()) - 1; i >= 0; --i) {
            int depth = static_cast<int>(join_lines.size()) - i;
            lines.push_back(std::string(depth, '\t') + join_lines[i]);
        }
    }

    bool has_filters = false;
    for (const auto &visible : input_visible) has_filters = has_filters || table_has_filter(explain_conds, visible);
    if (join_conds.empty() && has_filters) {
        std::sort(leaf_order.begin(), leaf_order.end(), [&](const std::string &lhs, const std::string &rhs) {
            return alias_to_table.at(lhs) < alias_to_table.at(rhs);
        });
    }
    for (const auto &visible : input_visible) {
        if (std::find(leaf_order.begin(), leaf_order.end(), visible) == leaf_order.end()) leaf_order.push_back(visible);
    }

    for (const auto &visible : leaf_order) {
        std::vector<std::string> projected;
        if (!select->cols.empty()) {
            for (const auto &col : select->cols) {
                if (col && !col->is_aggregate && col->tab_name == visible) projected.push_back(col_to_string(col));
            }
            for (const auto &cond : join_conds) {
                auto rhs_col = std::dynamic_pointer_cast<ast::Col>(cond->rhs);
                if (cond->lhs->tab_name == visible) projected.push_back(col_to_string(cond->lhs));
                if (rhs_col && rhs_col->tab_name == visible) projected.push_back(col_to_string(rhs_col));
            }
            std::sort(projected.begin(), projected.end());
            projected.erase(std::unique(projected.begin(), projected.end()), projected.end());
            if (!projected.empty() && !join_conds.empty()) {
                int rows = leaf_output_rows.count(visible) ? leaf_output_rows[visible] : filtered_rows(visible);
                lines.push_back(std::string(leaf_depth[visible], '\t') + "Project(columns=[" +
                                join_strings(projected) + "], rows=" + std::to_string(rows) + ")");
            }
        }

        std::string filter = table_filter_string(explain_conds, visible);
        if (!filter.empty()) {
            int rows = filtered_rows(visible) * (join_conds.empty() ? 1 : leaf_multiplier[visible]);
            int depth = join_conds.empty() ? 1 : leaf_depth[visible] + (projected.empty() ? 0 : 1);
            lines.push_back(std::string(depth, '\t') + "Filter(condition=[" + filter + "], rows=" +
                            std::to_string(rows) + ")");
        }

        int rows = leaf_index[visible] ? leaf_output_rows[visible]
                                       : table_rows(visible) * (join_conds.empty() ? 1 : leaf_multiplier[visible]);
        std::string scan = "Scan(table=" + alias_to_table.at(visible) + ", type=" +
                           (leaf_index[visible] ? "IndexScan" : "SeqScan");
        if (leaf_index[visible]) {
            auto index_cond = leaf_index_cond[visible];
            auto rhs_col = std::dynamic_pointer_cast<ast::Col>(index_cond->rhs);
            if (index_cond->lhs->tab_name == visible) scan += ", using_index=(" + index_cond->lhs->col_name + ")";
            else if (rhs_col && rhs_col->tab_name == visible) scan += ", using_index=(" + rhs_col->col_name + ")";
        }
        scan += ", rows=" + std::to_string(rows) + ")";
        int depth = join_conds.empty() ? (filter.empty() ? 1 : 2)
                                       : leaf_depth[visible] + (projected.empty() ? 0 : 1) +
                                             (filter.empty() ? 0 : 1);
        lines.push_back(std::string(depth, '\t') + scan);
    }
    return lines;
}
