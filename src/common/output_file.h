#pragma once

#include <fstream>
#include <mutex>
#include <string>

// 开关属于 SessionTxnState/Context；这里只有跨会话追加同一文件所需的短临界区。
inline std::mutex output_file_mutex;

inline void append_output_file(const std::string &text, bool enabled) {
    if (!enabled) return;
    std::lock_guard<std::mutex> lock(output_file_mutex);
    std::ofstream outfile("output.txt", std::ios::out | std::ios::app);
    outfile << text;
}
