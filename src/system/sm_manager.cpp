/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sm_manager.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_set>

#include "common/output_file.h"
#include "execution/index_helper.h"
#include "index/ix.h"
#include "record/rm.h"
#include "record_printer.h"

namespace {

std::vector<std::string> ParseCsvRow(const std::string &line) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char ch = line[i];
        if (quoted) {
            if (ch == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    field.push_back('"');
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field.push_back(ch);
            }
        } else if (ch == ',' ) {
            fields.push_back(std::move(field));
            field.clear();
        } else if (ch == '"' && field.empty()) {
            quoted = true;
        } else {
            field.push_back(ch);
        }
    }
    if (quoted) {
        throw RMDBError("Unterminated quoted CSV field");
    }
    if (!field.empty() && field.back() == '\r') {
        field.pop_back();
    }
    fields.push_back(std::move(field));
    return fields;
}

int ParseIntField(const std::string &text) {
    if (text.empty()) throw RMDBError("Empty integer field in CSV");
    char *end = nullptr;
    errno = 0;
    long value = std::strtol(text.c_str(), &end, 10);
    if (errno == ERANGE || end != text.c_str() + text.size() ||
        value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
        throw RMDBError("Invalid integer field in CSV: " + text);
    }
    return static_cast<int>(value);
}

float ParseFloatField(const std::string &text) {
    if (text.empty()) throw RMDBError("Empty float field in CSV");
    char *end = nullptr;
    errno = 0;
    float value = std::strtof(text.c_str(), &end);
    if (errno == ERANGE || end != text.c_str() + text.size()) {
        throw RMDBError("Invalid float field in CSV: " + text);
    }
    return value;
}

}  // namespace

/**
 * @description: 判断是否为一个文件夹
 * @return {bool} 返回是否为一个文件夹
 * @param {string&} db_name 数据库文件名称，与文件夹同名
 */
bool SmManager::is_dir(const std::string& db_name) {
    struct stat st;
    return stat(db_name.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/**
 * @description: 创建数据库，所有的数据库相关文件都放在数据库同名文件夹下
 * @param {string&} db_name 数据库名称
 */
void SmManager::create_db(const std::string& db_name) {
    if (is_dir(db_name)) {
        throw DatabaseExistsError(db_name);
    }
    // 为数据库创建一个子目录，避免通过 shell 拼接数据库名
    if (mkdir(db_name.c_str(), 0755) < 0) {
        throw UnixError();
    }
    if (chdir(db_name.c_str()) < 0) {  // 进入名为db_name的目录
        throw UnixError();
    }
    //创建系统目录
    DbMeta new_db;
    new_db.name_ = db_name;

    // 注意，此处ofstream会在当前目录创建(如果没有此文件先创建)和打开一个名为DB_META_NAME的文件
    std::ofstream ofs(DB_META_NAME);

    // 将new_db中的信息，按照定义好的operator<<操作符，写入到ofs打开的DB_META_NAME文件中
    ofs << new_db;  // 注意：此处重载了操作符<<

    // 创建日志文件
    disk_manager_->create_file(LOG_FILE_NAME);

    // 回到根目录
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 删除数据库，同时需要清空相关文件以及数据库同名文件夹
 * @param {string&} db_name 数据库名称，与文件夹同名
 */
void SmManager::drop_db(const std::string& db_name) {
    if (!is_dir(db_name)) {
        throw DatabaseNotFoundError(db_name);
    }
    std::string cmd = "rm -r " + db_name;
    if (system(cmd.c_str()) < 0) {
        throw UnixError();
    }
}

/**
 * @description: 打开数据库，找到数据库对应的文件夹，并加载数据库元数据和相关文件
 * @param {string&} db_name 数据库名称，与文件夹同名
 */
void SmManager::open_db(const std::string& db_name) {
    if (!is_dir(db_name)) {
        throw DatabaseNotFoundError(db_name);
    }
    // 打开数据库时进入数据库目录，后续表文件、索引文件和日志文件都基于当前目录访问
    if (chdir(db_name.c_str()) < 0) {
        throw UnixError();
    }

    // 从元数据文件恢复数据库中所有表和索引的定义
    std::ifstream ifs(DB_META_NAME);
    db_ = DbMeta();
    ifs >> db_;

    // 根据表元数据打开每张表的数据文件，执行器后续通过 fhs_ 直接访问
    for (auto &entry : db_.tabs_) {
        auto &tab = entry.second;
        fhs_.emplace(tab.name, rm_manager_->open_file(tab.name));

        // 根据索引元数据打开每个索引文件，键名保持和 IxManager 的索引文件名一致
        for (auto &index : tab.indexes) {
            std::string index_name = ix_manager_->get_index_name(tab.name, index.cols);
            ihs_.emplace(index_name, ix_manager_->open_index(tab.name, index.cols));
        }
    }

}

/**
 * @description: 把数据库相关的元数据刷入磁盘中
 */
void SmManager::flush_meta() {
    // 默认清空文件
    std::ofstream ofs(DB_META_NAME);
    ofs << db_;
}

/**
 * @description: 关闭数据库并把数据落盘
 */
void SmManager::close_db() {
    // 关闭前先落盘数据库元数据，保证表和索引定义不会丢失
    flush_meta();

    // 先关闭索引文件，把索引文件头和缓冲池中的索引页刷回磁盘
    for (auto &entry : ihs_) {
        ix_manager_->close_index(entry.second.get());
    }
    ihs_.clear();

    // 再关闭表数据文件，把表文件头和缓冲池中的数据页刷回磁盘
    for (auto &entry : fhs_) {
        rm_manager_->close_file(entry.second.get());
    }
    fhs_.clear();

    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 显示所有的表,通过测试需要将其结果写入到output.txt,详情看题目文档
 * @param {Context*} context 
 */
void SmManager::show_tables(Context* context) {
    const bool write_output = context->output_file_enabled_;
    std::unique_lock<std::mutex> output_lock(output_file_mutex, std::defer_lock);
    std::fstream outfile;
    if (write_output) {
        output_lock.lock();
        outfile.open("output.txt", std::ios::out | std::ios::app);
        outfile << "| Tables |\n";
    }
    RecordPrinter printer(1);
    printer.print_separator(context);
    printer.print_record({"Tables"}, context);
    printer.print_separator(context);
    for (auto &entry : db_.tabs_) {
        auto &tab = entry.second;
        printer.print_record({tab.name}, context);
        if (write_output) outfile << "| " << tab.name << " |\n";
    }
    printer.print_separator(context);
    outfile.close();
}

void SmManager::show_index(const std::string& tab_name, Context* context) {
    const bool write_output = context->output_file_enabled_;
    std::unique_lock<std::mutex> output_lock(output_file_mutex, std::defer_lock);
    TabMeta &tab = db_.get_table(tab_name);

    std::fstream outfile;
    if (write_output) {
        output_lock.lock();
        outfile.open("output.txt", std::ios::out | std::ios::app);
    }

    for (auto &index : tab.indexes) {
        std::string index_cols = "(";
        for (int i = 0; i < index.col_num; ++i) {
            if (i > 0) {
                index_cols += ",";
            }
            index_cols += index.cols[i].name;
        }
        index_cols += ")";

        std::string line = "| " + tab.name + " | unique | " + index_cols + " |\n";
        context->AppendResponse(line);
        if (write_output) outfile << line;
    }

    outfile.close();
}

void SmManager::load_data(const std::string &file_name, const std::string &tab_name) {
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }

    std::ifstream input(file_name);
    if (!input.is_open()) {
        throw RMDBError("Cannot open load file: " + file_name);
    }

    TabMeta &tab = db_.get_table(tab_name);
    RmFileHandle *file_handle = fhs_.at(tab_name).get();
    // bulk_insert_records 会在整个批量装载期间持有 heap 元数据锁，
    // 因此必须提前缓存不可变的记录长度，避免回调中再次获取同一把锁。
    const int record_size = file_handle->get_file_hdr().record_size;
    std::string line;
    if (!std::getline(input, line)) {
        throw RMDBError("Load file is empty: " + file_name);
    }

    auto header = ParseCsvRow(line);
    if (header.size() != tab.cols.size()) {
        throw InvalidValueCountError();
    }
    for (size_t i = 0; i < header.size(); ++i) {
        if (header[i] != tab.cols[i].name) {
            throw RMDBError("CSV header does not match table schema");
        }
    }

    size_t line_no = 1;
    auto read_record = [&](char *record_data) {
        while (std::getline(input, line)) {
            ++line_no;
            if (line.empty() || line == "\r") continue;
            auto fields = ParseCsvRow(line);
            if (fields.size() != tab.cols.size()) {
                throw RMDBError("Invalid CSV field count at line " + std::to_string(line_no));
            }
            memset(record_data, 0, record_size);
            for (size_t i = 0; i < fields.size(); ++i) {
                const ColMeta &col = tab.cols[i];
                char *dest = record_data + col.offset;
                if (col.type == TYPE_INT) {
                    int value = ParseIntField(fields[i]);
                    memcpy(dest, &value, sizeof(value));
                } else if (col.type == TYPE_FLOAT) {
                    float value = ParseFloatField(fields[i]);
                    memcpy(dest, &value, sizeof(value));
                } else {
                    if (fields[i].size() > static_cast<size_t>(col.len)) {
                        throw StringOverflowError();
                    }
                    memcpy(dest, fields[i].data(), fields[i].size());
                }
            }
            return true;
        }
        return false;
    };

    struct BulkIndex {
        const IndexMeta *meta;
        IxIndexHandle *handle;
    };
    std::vector<BulkIndex> indexes;
    indexes.reserve(tab.indexes.size());
    for (const auto &index : tab.indexes) {
        auto *ih = ihs_.at(ix_manager_->get_index_name(tab_name, index.cols)).get();
        indexes.push_back(BulkIndex{&index, ih});
    }

    if (indexes.empty()) {
        file_handle->bulk_insert_records(read_record);
    } else {
        RmRecord record(record_size);
        while (read_record(record.data)) {
            std::vector<std::pair<IxIndexHandle *, std::vector<char>>> index_entries;
            index_entries.reserve(indexes.size());
            for (const auto &index : indexes) {
                auto key = IndexHelper::MakeKey(*index.meta, record);
                Rid existing;
                if (index.handle->get_value(key.data(), &existing, nullptr)) throw DuplicateKeyError();
                index_entries.emplace_back(index.handle, std::move(key));
            }

            Rid rid = file_handle->insert_record(record.data, nullptr);
            for (auto &entry : index_entries) {
                entry.first->insert_entry(entry.second.data(), rid, nullptr, INVALID_LSN, false);
            }
        }
    }

    file_handle->flush_file_hdr();
    for (const auto &index : indexes) {
        index.handle->flush_file_hdr();
    }
    buffer_pool_manager_->flush_all_pages(file_handle->GetFd());
    for (const auto &index : indexes) {
        buffer_pool_manager_->flush_all_pages(index.handle->GetFd());
    }
}

/**
 * @description: 显示表的元数据
 * @param {string&} tab_name 表名称
 * @param {Context*} context 
 */
void SmManager::desc_table(const std::string& tab_name, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);

    std::vector<std::string> captions = {"Field", "Type", "Index"};
    RecordPrinter printer(captions.size());
    // Print header
    printer.print_separator(context);
    printer.print_record(captions, context);
    printer.print_separator(context);
    // Print fields
    for (auto &col : tab.cols) {
        std::vector<std::string> field_info = {col.name, coltype2str(col.type), col.index ? "YES" : "NO"};
        printer.print_record(field_info, context);
    }
    // Print footer
    printer.print_separator(context);
}

/**
 * @description: 创建表
 * @param {string&} tab_name 表的名称
 * @param {vector<ColDef>&} col_defs 表的字段
 * @param {Context*} context 
 */
void SmManager::create_table(const std::string& tab_name, const std::vector<ColDef>& col_defs, Context* context) {
    if (db_.is_table(tab_name)) {
        throw TableExistsError(tab_name);
    }
    // Create table meta
    int curr_offset = 0;
    TabMeta tab;
    tab.name = tab_name;
    std::unordered_set<std::string> col_names;
    for (auto &col_def : col_defs) {
        if (!col_names.insert(col_def.name).second) {
            throw RMDBError("Duplicate column: " + col_def.name);
        }
        ColMeta col = {.tab_name = tab_name,
                       .name = col_def.name,
                       .type = col_def.type,
                       .len = col_def.len,
                       .offset = curr_offset,
                       .index = false};
        curr_offset += col_def.len;
        tab.cols.push_back(col);
    }
    // Create & open record file
    int record_size = curr_offset;  // record_size就是col meta所占的大小（表的元数据也是以记录的形式进行存储的）
    rm_manager_->create_file(tab_name, record_size);
    db_.tabs_[tab_name] = tab;
    // fhs_[tab_name] = rm_manager_->open_file(tab_name);
    fhs_.emplace(tab_name, rm_manager_->open_file(tab_name));

    flush_meta();
}

/**
 * @description: 删除表
 * @param {string&} tab_name 表的名称
 * @param {Context*} context
 */
void SmManager::drop_table(const std::string& tab_name, Context* context) {
    if (!db_.is_table(tab_name)) {
        throw TableNotFoundError(tab_name);
    }

    // drop_index 会修改 tab.indexes，因此先复制一份索引列表再遍历
    auto indexes = db_.get_table(tab_name).indexes;
    for (auto &index : indexes) {
        drop_index(tab_name, index.cols, context);
    }

    // 表文件仍打开时需要先关闭并刷盘，否则 DiskManager 会拒绝删除文件
    auto fh = fhs_.find(tab_name);
    if (fh != fhs_.end()) {
        rm_manager_->close_file(fh->second.get());
        fhs_.erase(fh);
    }
    rm_manager_->destroy_file(tab_name);

    // 最后删除内存元数据并写回 db.meta，使元数据与磁盘文件保持一致
    db_.tabs_.erase(tab_name);
    flush_meta();
}

/**
 * @description: 创建索引
 * @param {string&} tab_name 表的名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context
 */
void SmManager::create_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);
    if (tab.is_index(col_names)){
        throw IndexExistsError(tab.name,col_names);
    }

    IndexMeta index_meta{};
    index_meta.tab_name = tab.name;
    index_meta.col_num = col_names.size();
    // 根据字段名收集索引列元数据，并计算复合 key 的总长度
    for (auto &col_name : col_names) {
        auto col = tab.get_col(col_name);
        index_meta.cols.push_back(*col);
        index_meta.col_tot_len += col->len;
    }

    // 创建并打开索引文件，后续执行器通过 ihs_ 复用该句柄
    ix_manager_->create_index(tab_name, index_meta.cols);
    std::string index_name = ix_manager_->get_index_name(tab_name, index_meta.cols);
    auto ih = ix_manager_->open_index(tab_name, index_meta.cols);

    // 建索引时需要把表中已有记录全部写入 B+ 树，保证新索引立刻可用
    auto file_handle = fhs_.at(tab_name).get();
    std::vector<char> key(index_meta.col_tot_len);
    try {
        // 先把已有记录全部写入临时打开的索引句柄；全部成功后才登记元数据
        for (RmScan scan(file_handle); !scan.is_end(); scan.next()) {
            Rid rid = scan.rid();
            auto record = file_handle->get_record(rid, context);
            int offset = 0;
            for (auto &col : index_meta.cols) {
                memcpy(key.data() + offset, record->data + col.offset, col.len);
                offset += col.len;
            }
            ih->insert_entry(key.data(), rid, context->txn_);
        }
    } catch (...) {
        // 建索引中途失败时，索引尚未进入 ihs_ 和 db.meta，直接关闭并删除孤儿文件
        ix_manager_->close_index(ih.get());
        ix_manager_->destroy_index(tab_name, index_meta.cols);
        throw;
    }

    ihs_.emplace(index_name, std::move(ih));
    tab.indexes.push_back(index_meta);
    // 标记参与索引的列，desc table 可以展示索引状态
    for (auto &col : index_meta.cols) {
        tab.get_col(col.name)->index = true;
    }

    flush_meta();
}

/**
 * @description: 删除索引
 * @param {string&} tab_name 表名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);
    // 先通过字段名定位索引元数据，保证只删除用户指定的这个索引
    auto index_meta = tab.get_index_meta(col_names);
    std::vector<ColMeta> cols = index_meta->cols;
    drop_index(tab_name, cols, context);
}

/**
 * @description: 删除索引
 * @param {string&} tab_name 表名称
 * @param {vector<ColMeta>&} 索引包含的字段元数据
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string& tab_name, const std::vector<ColMeta>& cols, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);
    std::string index_name = ix_manager_->get_index_name(tab_name, cols);

    // 索引文件处于打开状态时必须先关闭，再从磁盘删除对应文件
    auto ih = ihs_.find(index_name);
    if (ih != ihs_.end()) {
        ix_manager_->close_index(ih->second.get());
        ihs_.erase(ih);
    }
    ix_manager_->destroy_index(tab_name, cols);

    // 从表元数据中移除对应索引，后续优化器就不会再选择它
    std::vector<std::string> col_names;
    col_names.reserve(cols.size());
    for (auto &col : cols) {
        col_names.push_back(col.name);
    }
    tab.indexes.erase(tab.get_index_meta(col_names));

    // 重新计算列上的 index 标记，避免某列仍被其他索引使用时被误清零
    for (auto &col : tab.cols) {
        col.index = false;
    }
    for (auto &index : tab.indexes) {
        for (auto &index_col : index.cols) {
            tab.get_col(index_col.name)->index = true;
        }
    }

    flush_meta();
}
