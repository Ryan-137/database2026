#pragma once

#include <array>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "index/ix.h"
#include "system/sm.h"
#include "transaction/transaction_manager.h"

namespace MvccIndexProbe {

// 点查和短范围结果优先保存在对象内；只有超过内联容量时才申请堆内存。
class CandidateRids {
   public:
    CandidateRids() = default;
    CandidateRids(CandidateRids &&) noexcept = default;
    CandidateRids &operator=(CandidateRids &&) noexcept = default;
    CandidateRids(const CandidateRids &) = delete;
    CandidateRids &operator=(const CandidateRids &) = delete;

    void push_back(const Rid &rid) {
        if (!overflow_active_ && inline_size_ < inline_rids_.size()) {
            inline_rids_[inline_size_++] = rid;
            return;
        }
        if (!overflow_active_) {
            overflow_rids_.reserve(inline_rids_.size() * 2);
            overflow_rids_.insert(overflow_rids_.end(), inline_rids_.begin(),
                                  inline_rids_.begin() + static_cast<std::ptrdiff_t>(inline_size_));
            overflow_active_ = true;
        }
        overflow_rids_.push_back(rid);
    }

    void clear() {
        inline_size_ = 0;
        overflow_rids_.clear();
        overflow_active_ = false;
    }

    size_t size() const { return overflow_active_ ? overflow_rids_.size() : inline_size_; }
    bool empty() const { return size() == 0; }

    const Rid &operator[](size_t index) const {
        return overflow_active_ ? overflow_rids_[index] : inline_rids_[index];
    }

   private:
    std::array<Rid, 4> inline_rids_{};
    size_t inline_size_{0};
    std::vector<Rid> overflow_rids_;
    bool overflow_active_{false};
};

struct SkipScanCandidates {
    std::vector<Rid> rids;
    bool fallback_to_seq_scan{false};
    size_t prefix_count{0};
};

inline std::vector<ColType> IndexColTypes(const IndexMeta &index_meta) {
    std::vector<ColType> types;
    types.reserve(index_meta.cols.size());
    for (const auto &col : index_meta.cols) {
        types.push_back(col.type);
    }
    return types;
}

inline std::vector<int> IndexColLens(const IndexMeta &index_meta) {
    std::vector<int> lens;
    lens.reserve(index_meta.cols.size());
    for (const auto &col : index_meta.cols) {
        lens.push_back(col.len);
    }
    return lens;
}

inline void AddCandidate(std::vector<Rid> &candidates, std::set<std::pair<int, int>> &seen, const Rid &rid) {
    if (seen.emplace(rid.page_no, rid.slot_no).second) {
        candidates.push_back(rid);
    }
}

inline std::uint64_t EncodedRid(const Rid &rid) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(rid.page_no)) << 32U) |
           static_cast<std::uint32_t>(rid.slot_no);
}

inline CandidateRids CollectCandidates(SmManager *sm_manager, TransactionManager *txn_mgr,
                                       const std::string &tab_name, RmFileHandle *fh,
                                       const IndexMeta &index_meta, int index_no,
                                       const std::vector<char> &lower_key,
                                       const std::vector<char> &upper_key,
                                       Transaction *reader) {
    CandidateRids candidates;
    std::unordered_set<std::uint64_t> seen;
    bool seen_ready = false;

    auto ensure_seen = [&]() {
        if (seen_ready) {
            return;
        }
        seen.reserve(candidates.size() * 2 + 1);
        for (size_t i = 0; i < candidates.size(); ++i) {
            seen.insert(EncodedRid(candidates[i]));
        }
        seen_ready = true;
    };

    auto ih = sm_manager->ihs_.at(sm_manager->get_ix_manager()->get_index_name(tab_name, index_meta.cols)).get();
    auto collect_current_tree = [&](bool deduplicate) {
        if (lower_key == upper_key) {
            Rid current;
            if (ih->get_value(lower_key.data(), &current, reader) &&
                (!deduplicate || seen.insert(EncodedRid(current)).second)) {
                candidates.push_back(current);
            }
        } else {
            IxScan current_scan(ih, lower_key.data(), upper_key.data(), sm_manager->get_bpm());
            while (!current_scan.is_end()) {
                // 首扫直接追加保持零哈希开销；handoff 重扫用 RID hash 去重，避免
                // 大范围候选在第二轮退化成 O(N^2) 的线性 Contains。
                if (!deduplicate || seen.insert(EncodedRid(current_scan.rid())).second) {
                    candidates.push_back(current_scan.rid());
                }
                current_scan.next();
            }
        }
    };
    auto collect_stale = [&]() {
        std::vector<Rid> stale_rids;
        if (lower_key == upper_key) {
            stale_rids = txn_mgr->LookupStaleIndexEqual(fh->GetFd(), index_no, lower_key, reader);
        } else {
            stale_rids = txn_mgr->LookupStaleIndexRange(fh->GetFd(), index_no, lower_key, upper_key,
                                                        IndexColTypes(index_meta), IndexColLens(index_meta), reader);
        }
        if (stale_rids.empty()) {
            return;
        }
        ensure_seen();
        for (const auto &rid : stale_rids) {
            if (seen.insert(EncodedRid(rid)).second) {
                candidates.push_back(rid);
            }
        }
    };

    // abort 的顺序是“补回旧 B+ 键 -> 发布 epoch -> 删除 stale”。必须在同一个
    // 稳定 epoch 内完成整组 tree+stale 探测：只补一次树扫描仍可能被紧接着的下一轮
    // UPDATE 再次切到 stale。稳态首轮保持 O(N)，只有观察到 handoff 才完整重试并去重。
    std::uint64_t handoff_epoch = txn_mgr->GetStaleIndexHandoffEpoch(fh->GetFd(), index_no);
    bool deduplicate_tree = false;
    while (true) {
        collect_current_tree(deduplicate_tree);
        collect_stale();
        std::uint64_t after = txn_mgr->GetStaleIndexHandoffEpoch(fh->GetFd(), index_no);
        if (after == handoff_epoch) {
            break;
        }
        handoff_epoch = after;
        ensure_seen();
        deduplicate_tree = true;
    }
    return candidates;
}

inline SkipScanCandidates CollectSkipScanCandidates(
    SmManager *sm_manager, TransactionManager *txn_mgr, const std::string &tab_name, RmFileHandle *fh,
    const IndexMeta &index_meta, int index_no, size_t leading_col_len,
    const std::vector<char> &lower_template, const std::vector<char> &upper_template,
    const std::vector<char> &full_lower_key, const std::vector<char> &full_upper_key,
    size_t max_prefixes, size_t max_candidates, Transaction *reader) {
    SkipScanCandidates result;
    std::set<std::pair<int, int>> seen;
    auto ih = sm_manager->ihs_.at(sm_manager->get_ix_manager()->get_index_name(tab_name, index_meta.cols)).get();

    auto collect_current_tree = [&]() -> bool {
        std::vector<char> probe_key = full_lower_key;
        bool strict_upper = false;
        size_t phase_prefix_count = 0;
        while (true) {
            std::vector<char> current_key;
            if (!ih->get_bound_key(probe_key.data(), strict_upper, &current_key)) {
                break;
            }
            if (phase_prefix_count >= max_prefixes) {
                // 前导列基数超过预算时丢弃已收集候选，改走堆扫描，避免 skip-scan 退化成大量树探测。
                result.rids.clear();
                result.fallback_to_seq_scan = true;
                return false;
            }
            phase_prefix_count++;

            std::vector<char> lower_key = lower_template;
            std::vector<char> upper_key = upper_template;
            memcpy(lower_key.data(), current_key.data(), leading_col_len);
            memcpy(upper_key.data(), current_key.data(), leading_col_len);

            IxScan current_scan(ih, lower_key.data(), upper_key.data(), sm_manager->get_bpm());
            while (!current_scan.is_end()) {
                AddCandidate(result.rids, seen, current_scan.rid());
                if (result.rids.size() > max_candidates) {
                    result.rids.clear();
                    result.fallback_to_seq_scan = true;
                    return false;
                }
                current_scan.next();
            }

            // 使用“当前前导值 + 后缀 MAX”的严格上界，一次树探测跳到下一个不同前导值。
            memcpy(probe_key.data(), current_key.data(), leading_col_len);
            memcpy(probe_key.data() + leading_col_len, full_upper_key.data() + leading_col_len,
                   probe_key.size() - leading_col_len);
            strict_upper = true;
        }
        if (phase_prefix_count > result.prefix_count) {
            result.prefix_count = phase_prefix_count;
        }
        return true;
    };

    auto collect_stale = [&]() -> bool {
        // 旧快照可能只在 stale registry 中保留某个已消失的前导值，因此必须扫描该
        // 索引的全部 stale key，再由执行器做 MVCC 可见性与完整谓词复核。
        auto stale_lookup = txn_mgr->LookupStaleIndexRangeBounded(
            fh->GetFd(), index_no, full_lower_key, full_upper_key,
            IndexColTypes(index_meta), IndexColLens(index_meta), max_candidates, reader);
        if (stale_lookup.second) {
            result.rids.clear();
            result.fallback_to_seq_scan = true;
            return false;
        }
        for (const auto &rid : stale_lookup.first) {
            AddCandidate(result.rids, seen, rid);
            if (result.rids.size() > max_candidates) {
                result.rids.clear();
                result.fallback_to_seq_scan = true;
                return false;
            }
        }
        return true;
    };

    // 与普通索引候选相同，必须在同一稳定 handoff epoch 内完成 tree+stale。
    // 固定的 tree/stale/tree 仍可能被紧接着的下一轮 UPDATE/abort 再次穿透。
    std::uint64_t handoff_epoch = txn_mgr->GetStaleIndexHandoffEpoch(fh->GetFd(), index_no);
    while (true) {
        if (!collect_current_tree() || !collect_stale()) {
            return result;
        }
        std::uint64_t after = txn_mgr->GetStaleIndexHandoffEpoch(fh->GetFd(), index_no);
        if (after == handoff_epoch) {
            break;
        }
        handoff_epoch = after;
    }
    return result;
}

}  // namespace MvccIndexProbe
