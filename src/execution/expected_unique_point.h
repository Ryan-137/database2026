#pragma once

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "common/common.h"
#include "record/rm_defs.h"
#include "system/sm_meta.h"
#include "transaction/txn_defs.h"

inline std::optional<UniqueKeyId> BuildExpectedUniquePointKey(
    int fd, const TabMeta &tab, const std::vector<Condition> &conds) {
    for (size_t index_no = 0; index_no < tab.indexes.size(); ++index_no) {
        const auto &index = tab.indexes[index_no];
        std::string encoded_key;
        encoded_key.resize(index.col_tot_len);
        int offset = 0;
        bool full_match = true;
        for (const auto &index_col : index.cols) {
            auto cond_it = std::find_if(conds.begin(), conds.end(), [&](const Condition &cond) {
                const bool same_table = cond.lhs_col.tab_name.empty() || cond.lhs_col.tab_name == tab.name;
                return same_table && cond.lhs_col.col_name == index_col.name &&
                       cond.op == OP_EQ && cond.is_rhs_val;
            });
            if (cond_it == conds.end()) {
                full_match = false;
                break;
            }
            Value rhs = cond_it->rhs_val;
            if (rhs.raw == nullptr) {
                rhs.init_raw(index_col.len);
            }
            std::memcpy(encoded_key.data() + offset, rhs.raw->data, index_col.len);
            offset += index_col.len;
        }
        if (full_match) {
            return UniqueKeyId{fd, static_cast<int>(index_no), std::move(encoded_key)};
        }
    }
    return std::nullopt;
}

inline bool RecordMatchesExpectedUniquePoint(const TabMeta &tab, const RmRecord &record,
                                             const UniqueKeyId &expected) {
    if (expected.index_no < 0 || static_cast<size_t>(expected.index_no) >= tab.indexes.size()) {
        return false;
    }
    const auto &index = tab.indexes[expected.index_no];
    if (expected.encoded_key.size() != static_cast<size_t>(index.col_tot_len)) {
        return false;
    }
    size_t key_offset = 0;
    for (const auto &col : index.cols) {
        if (std::memcmp(record.data + col.offset, expected.encoded_key.data() + key_offset, col.len) != 0) {
            return false;
        }
        key_offset += static_cast<size_t>(col.len);
    }
    return true;
}
