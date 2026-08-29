#pragma once

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "executor_abstract.h"

class UnionExecutor : public AbstractExecutor {
private:
    std::vector<std::unique_ptr<AbstractExecutor>> children_;
    std::vector<ColMeta> cols_;
    size_t len_ = 0;
    std::vector<RmRecord> records_;
    size_t pos_ = 0;

    RmRecord convert_record(const RmRecord &input, const std::vector<ColMeta> &input_cols) const {
        if (input_cols.size() != cols_.size()) throw InternalError("UNION executor schema mismatch");
        RmRecord output(static_cast<int>(len_));
        memset(output.data, 0, len_);
        for (size_t i = 0; i < cols_.size(); ++i) {
            const auto &src = input_cols[i];
            const auto &dst = cols_[i];
            const char *src_data = input.data + src.offset;
            char *dst_data = output.data + dst.offset;
            if (src.type == dst.type) {
                memcpy(dst_data, src_data, std::min(src.len, dst.len));
            } else if (src.type == TYPE_INT && dst.type == TYPE_FLOAT) {
                float value = static_cast<float>(*reinterpret_cast<const int *>(src_data));
                memcpy(dst_data, &value, sizeof(value));
            } else {
                throw InternalError("Unsupported UNION type conversion");
            }
        }
        return output;
    }

public:
    UnionExecutor(std::vector<std::unique_ptr<AbstractExecutor>> children, std::vector<ColMeta> output_cols)
        : children_(std::move(children)), cols_(std::move(output_cols)) {
        for (const auto &col : cols_) len_ = std::max(len_, static_cast<size_t>(col.offset + col.len));
    }

    size_t tupleLen() const override { return len_; }
    const std::vector<ColMeta> &cols() const override { return cols_; }
    std::string getType() override { return "UnionExecutor"; }

    void beginTuple() override {
        records_.clear();
        pos_ = 0;
        std::unordered_set<std::string> seen;
        for (auto &child : children_) {
            for (child->beginTuple(); !child->is_end(); child->nextTuple()) {
                std::unique_ptr<RmRecord> owned_input;
                const RmRecord *input = child->CurrentOrNext(&owned_input);
                if (input == nullptr) continue;
                auto output = convert_record(*input, child->cols());
                std::string key(output.data, len_);
                if (seen.insert(key).second) records_.emplace_back(std::move(output));
            }
        }
    }

    void nextTuple() override { ++pos_; }
    bool is_end() const override { return pos_ >= records_.size(); }
    std::unique_ptr<RmRecord> Next() override {
        return is_end() ? nullptr : std::make_unique<RmRecord>(records_[pos_]);
    }
    const RmRecord *Current() const override { return is_end() ? nullptr : &records_[pos_]; }
    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }
    Rid &rid() override { return _abstract_rid; }
};
