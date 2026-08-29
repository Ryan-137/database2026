/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#undef NDEBUG

#define private public

#include "record/rm.h"
#include "storage/buffer_pool_manager.h"

#undef private

#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>  // NOLINT
#include <unordered_map>
#include <vector>

#include "gtest/gtest.h"
#include "replacer/lru_replacer.h"
#include "storage/disk_manager.h"

const std::string TEST_DB_NAME = "BufferPoolManagerTest_db";  // 以数据库名作为根目录
const std::string TEST_FILE_NAME = "basic";                   // 测试文件的名字
const std::string TEST_FILE_NAME_CCUR = "concurrency";        // 测试文件的名字
const std::string TEST_FILE_NAME_BIG = "bigdata";             // 测试文件的名字
constexpr int MAX_FILES = 32;
constexpr int MAX_PAGES = 128;
constexpr size_t TEST_BUFFER_POOL_SIZE = MAX_FILES * MAX_PAGES;

// 创建BufferPoolManager
auto disk_manager = std::make_unique<DiskManager>();
auto buffer_pool_manager = std::make_unique<BufferPoolManager>(TEST_BUFFER_POOL_SIZE, disk_manager.get());

std::unordered_map<int, char *> mock;  // fd -> buffer

char *mock_get_page(int fd, int page_no) { return &mock[fd][page_no * PAGE_SIZE]; }

void check_disk(int fd, int page_no) {
    char buf[PAGE_SIZE];
    disk_manager->read_page(fd, page_no, buf, PAGE_SIZE);
    char *mock_buf = mock_get_page(fd, page_no);
    assert(memcmp(buf, mock_buf, PAGE_SIZE) == 0);
}

void check_disk_all() {
    for (auto &file : mock) {
        int fd = file.first;
        for (int page_no = 0; page_no < MAX_PAGES; page_no++) {
            check_disk(fd, page_no);
        }
    }
}

void check_cache(int fd, int page_no) {
    Page *page = buffer_pool_manager->fetch_page(PageId{fd, page_no});
    char *mock_buf = mock_get_page(fd, page_no);  // &mock[fd][page_no * PAGE_SIZE];
    assert(memcmp(page->get_data(), mock_buf, PAGE_SIZE) == 0);
    buffer_pool_manager->unpin_page(PageId{fd, page_no}, false);
}

void check_cache_all() {
    for (auto &file : mock) {
        int fd = file.first;
        for (int page_no = 0; page_no < MAX_PAGES; page_no++) {
            check_cache(fd, page_no);
        }
    }
}

void rand_buf(int size, char *buf) {
    for (int i = 0; i < size; i++) {
        int rand_ch = rand() & 0xff;
        buf[i] = rand_ch;
    }
}

int rand_fd() {
    assert(mock.size() == MAX_FILES);
    int fd_idx = rand() % MAX_FILES;
    auto it = mock.begin();
    for (int i = 0; i < fd_idx; i++) {
        it++;
    }
    return it->first;
}

struct rid_hash_t {
    size_t operator()(const Rid &rid) const { return (rid.page_no << 16) | rid.slot_no; }
};

struct rid_equal_t {
    bool operator()(const Rid &x, const Rid &y) const { return x.page_no == y.page_no && x.slot_no == y.slot_no; }
};

void check_equal(const RmFileHandle *file_handle,
                 const std::unordered_map<Rid, std::string, rid_hash_t, rid_equal_t> &mock) {
    // Test all records
    for (auto &entry : mock) {
        Rid rid = entry.first;
        auto mock_buf = (char *)entry.second.c_str();
        auto rec = file_handle->get_record(rid, nullptr);
        assert(memcmp(mock_buf, rec->data, file_handle->file_hdr_.record_size) == 0);
    }
    // Randomly get record
    for (int i = 0; i < 10; i++) {
        Rid rid = {.page_no = 1 + rand() % (file_handle->file_hdr_.num_pages - 1),
                   .slot_no = rand() % file_handle->file_hdr_.num_records_per_page};
        bool mock_exist = mock.count(rid) > 0;
        bool rm_exist = file_handle->is_record(rid);
        assert(rm_exist == mock_exist);
    }
    // Test RM scan
    size_t num_records = 0;
    for (RmScan scan(file_handle); !scan.is_end(); scan.next()) {
        assert(mock.count(scan.rid()) > 0);
        auto rec = file_handle->get_record(scan.rid(), nullptr);
        assert(memcmp(rec->data, mock.at(scan.rid()).c_str(), file_handle->file_hdr_.record_size) == 0);
        num_records++;
    }
    assert(num_records == mock.size());
}

// std::cout can call this, for example: std::cout << rid
std::ostream &operator<<(std::ostream &os, const Rid &rid) {
    return os << '(' << rid.page_no << ", " << rid.slot_no << ')';
}

/** 注意：每个测试点只测试了单个文件！
 * 对于每个测试点，先创建和进入目录TEST_DB_NAME
 * 然后在此目录下创建和打开文件TEST_FILE_NAME_BIG，记录其文件描述符fd */

class BigStorageTest : public ::testing::Test {
   public:
    std::unique_ptr<DiskManager> disk_manager_;
    int fd_ = -1;  // 此文件描述符为disk_manager_->open_file的返回值

   public:
    // This function is called before every test.
    void SetUp() override {
        ::testing::Test::SetUp();
        // For each test, we create a new DiskManager
        disk_manager_ = std::make_unique<DiskManager>();
        // 如果测试目录不存在，则先创建测试目录
        if (!disk_manager_->is_dir(TEST_DB_NAME)) {
            disk_manager_->create_dir(TEST_DB_NAME);
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
        // 进入测试目录
        if (chdir(TEST_DB_NAME.c_str()) < 0) {
            throw UnixError();
        }
        // 如果测试文件存在，则先删除原文件（最后留下来的文件存的是最后一个测试点的数据）
        if (disk_manager_->is_file(TEST_FILE_NAME_BIG)) {
            disk_manager_->destroy_file(TEST_FILE_NAME_BIG);
        }
        // 创建测试文件
        disk_manager_->create_file(TEST_FILE_NAME_BIG);
        assert(disk_manager_->is_file(TEST_FILE_NAME_BIG));
        // 打开测试文件
        fd_ = disk_manager_->open_file(TEST_FILE_NAME_BIG);
        assert(fd_ != -1);
    }

    // This function is called after every test.
    void TearDown() override {
        disk_manager_->close_file(fd_);
        // disk_manager_->destroy_file(TEST_FILE_NAME_BIG);  // you can choose to delete the file

        // 返回上一层目录
        if (chdir("..") < 0) {
            throw UnixError();
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
    };
};

TEST(LRUReplacerTest, SampleTest) {
    LRUReplacer lru_replacer(7);

    // Scenario: unpin six elements, i.e. add them to the replacer.
    lru_replacer.unpin(1);
    lru_replacer.unpin(2);
    lru_replacer.unpin(3);
    lru_replacer.unpin(4);
    lru_replacer.unpin(5);
    lru_replacer.unpin(6);
    lru_replacer.unpin(1);
    EXPECT_EQ(6, lru_replacer.Size());

    // Scenario: get three victims from the lru.
    int value;
    lru_replacer.victim(&value);
    EXPECT_EQ(1, value);
    lru_replacer.victim(&value);
    EXPECT_EQ(2, value);
    lru_replacer.victim(&value);
    EXPECT_EQ(3, value);

    // Scenario: pin elements in the replacer.
    // Note that 3 has already been victimized, so pinning 3 should have no effect.
    lru_replacer.pin(3);
    lru_replacer.pin(4);
    EXPECT_EQ(2, lru_replacer.Size());

    // Scenario: unpin 4. We expect that the reference bit of 4 will be set to 1.
    lru_replacer.unpin(4);

    // Scenario: continue looking for victims. We expect these victims.
    lru_replacer.victim(&value);
    EXPECT_EQ(5, value);
    lru_replacer.victim(&value);
    EXPECT_EQ(6, value);
    lru_replacer.victim(&value);
    EXPECT_EQ(4, value);
}

/** 注意：每个测试点只测试了单个文件！
 * 对于每个测试点，先创建和进入目录TEST_DB_NAME
 * 然后在此目录下创建和打开文件TEST_FILE_NAME，记录其文件描述符fd */
class BufferPoolManagerTest : public ::testing::Test {
   public:
    std::unique_ptr<DiskManager> disk_manager_;
    int fd_ = -1;  // 此文件描述符为disk_manager_->open_file的返回值

   public:
    // This function is called before every test.
    void SetUp() override {
        ::testing::Test::SetUp();
        // For each test, we create a new DiskManager
        disk_manager_ = std::make_unique<DiskManager>();
        // 如果测试目录不存在，则先创建测试目录
        if (!disk_manager_->is_dir(TEST_DB_NAME)) {
            disk_manager_->create_dir(TEST_DB_NAME);
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
        // 进入测试目录
        if (chdir(TEST_DB_NAME.c_str()) < 0) {
            throw UnixError();
        }
        // 如果测试文件存在，则先删除原文件（最后留下来的文件存的是最后一个测试点的数据）
        if (disk_manager_->is_file(TEST_FILE_NAME)) {
            disk_manager_->destroy_file(TEST_FILE_NAME);
        }
        // 创建测试文件
        disk_manager_->create_file(TEST_FILE_NAME);
        assert(disk_manager_->is_file(TEST_FILE_NAME));
        // 打开测试文件
        fd_ = disk_manager_->open_file(TEST_FILE_NAME);
        assert(fd_ != -1);
    }

    // This function is called after every test.
    void TearDown() override {
        disk_manager_->close_file(fd_);
        // disk_manager_->destroy_file(TEST_FILE_NAME);  // you can choose to delete the file

        // 返回上一层目录
        if (chdir("..") < 0) {
            throw UnixError();
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
    };
};

// NOLINTNEXTLINE
TEST_F(BufferPoolManagerTest, SampleTest) {
    // create BufferPoolManager
    const size_t buffer_pool_size = 10;
    auto disk_manager = BufferPoolManagerTest::disk_manager_.get();
    auto bpm = std::make_unique<BufferPoolManager>(buffer_pool_size, disk_manager);
    // create tmp PageId
    int fd = BufferPoolManagerTest::fd_;
    PageId page_id_temp = {.fd = fd, .page_no = INVALID_PAGE_ID};
    auto *page0 = bpm->new_page(&page_id_temp);

    // Scenario: The buffer pool is empty. We should be able to create a new page.
    ASSERT_NE(nullptr, page0);
    EXPECT_EQ(0, page_id_temp.page_no);

    // Scenario: Once we have a page, we should be able to read and write content.
    snprintf(page0->get_data(), sizeof(page0->get_data()), "Hello");
    page0->mark_wal_free_dirty();
    EXPECT_EQ(0, strcmp(page0->get_data(), "Hello"));

    // Scenario: We should be able to create new pages until we fill up the buffer pool.
    for (size_t i = 1; i < buffer_pool_size; ++i) {
        Page *page = bpm->new_page(&page_id_temp);
        ASSERT_NE(nullptr, page);
        page->mark_wal_free_dirty();
    }

    // Scenario: Once the buffer pool is full, we should not be able to create any new pages.
    for (size_t i = buffer_pool_size; i < buffer_pool_size * 2; ++i) {
        EXPECT_EQ(nullptr, bpm->new_page(&page_id_temp));
    }

    // Scenario: After unpinning pages {0, 1, 2, 3, 4} and pinning another 4 new pages,
    // there would still be one cache frame left for reading page 0.
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(true, bpm->unpin_page(PageId{fd, i}, true));
    }
    for (int i = 0; i < 4; ++i) {
        EXPECT_NE(nullptr, bpm->new_page(&page_id_temp));
    }

    // Scenario: We should be able to fetch the data we wrote a while ago.
    page0 = bpm->fetch_page(PageId{fd, 0});
    EXPECT_EQ(0, strcmp(page0->get_data(), "Hello"));
    EXPECT_EQ(true, bpm->unpin_page(PageId{fd, 0}, false));
    // new_page again, and now all buffers are pinned. Page 0 would be failed to fetch.
    EXPECT_NE(nullptr, bpm->new_page(&page_id_temp));
    EXPECT_EQ(nullptr, bpm->fetch_page(PageId{fd, 0}));

    bpm->flush_all_pages(fd);
}

// NOLINTNEXTLINE
// M9: verify partition self-adaptive degeneration on tiny/odd pools.
// Invariants: no zero-frame partition, frame ranges are contiguous and cover
// [0, pool_size) exactly (no lost/overlapping frames), and basic
// new/fetch/unpin still works without crashing.
TEST_F(BufferPoolManagerTest, SmallPoolPartitionInvariant) {
    auto disk_manager = BufferPoolManagerTest::disk_manager_.get();
    int fd = BufferPoolManagerTest::fd_;
    for (size_t pool_size : {size_t(1), size_t(3), size_t(7), size_t(50)}) {
        auto bpm = std::make_unique<BufferPoolManager>(pool_size, disk_manager);
        // partition count must be >= 1 and never exceed pool_size
        ASSERT_GE(bpm->num_partitions_, size_t(1));
        ASSERT_LE(bpm->num_partitions_, pool_size);
        ASSERT_EQ(bpm->partitions_.size(), bpm->num_partitions_);
        // frame ranges must be contiguous, non-empty, and cover [0, pool_size)
        frame_id_t expected_begin = 0;
        size_t total_frames = 0;
        for (size_t i = 0; i < bpm->num_partitions_; ++i) {
            auto &part = bpm->partitions_[i];
            EXPECT_EQ(part.frame_begin, expected_begin);
            EXPECT_GT(part.frame_end, part.frame_begin) << "zero-frame partition at " << i;
            size_t part_frames = static_cast<size_t>(part.frame_end - part.frame_begin);
            EXPECT_EQ(part.free_list.size(), part_frames) << "free_list mismatch at " << i;
            total_frames += part_frames;
            expected_begin = part.frame_end;
        }
        EXPECT_EQ(total_frames, pool_size) << "lost/duplicated frames for pool_size=" << pool_size;
        EXPECT_EQ(static_cast<size_t>(expected_begin), pool_size);

        // Functional smoke: fill the pool, then over-allocate must fail cleanly.
        std::vector<PageId> allocated;
        for (size_t i = 0; i < pool_size; ++i) {
            PageId pid{fd, INVALID_PAGE_ID};
            Page *p = bpm->new_page(&pid);
            ASSERT_NE(nullptr, p) << "new_page failed at " << i << " for pool_size=" << pool_size;
            snprintf(p->get_data(), sizeof(p->get_data()), "sp-%d", pid.page_no);
            p->mark_wal_free_dirty();
            allocated.push_back(pid);
        }
        // All frames pinned -> further new_page must return nullptr, not crash.
        PageId overflow{fd, INVALID_PAGE_ID};
        EXPECT_EQ(nullptr, bpm->new_page(&overflow));
        // Unpin one, then fetch it back and verify contents survive.
        ASSERT_FALSE(allocated.empty());
        PageId first = allocated.front();
        EXPECT_TRUE(bpm->unpin_page(first, true));
        Page *refetch = bpm->fetch_page(first);
        ASSERT_NE(nullptr, refetch);
        char expect[32];
        snprintf(expect, sizeof(expect), "sp-%d", first.page_no);
        EXPECT_EQ(0, strcmp(refetch->get_data(), expect));
        EXPECT_TRUE(bpm->unpin_page(first, false));
        // release remaining pins so flush is clean
        for (size_t i = 1; i < allocated.size(); ++i) {
            bpm->unpin_page(allocated[i], false);
        }
        bpm->flush_all_pages(fd);
    }
}

/** 注意：每个测试点只测试了单个文件！
 * 对于每个测试点，先创建和进入目录TEST_DB_NAME
 * 然后在此目录下创建和打开文件TEST_FILE_NAME_CCUR，记录其文件描述符fd */

// Add by jiawen
class BufferPoolManagerConcurrencyTest : public ::testing::Test {
   public:
    std::unique_ptr<DiskManager> disk_manager_;
    int fd_ = -1;  // 此文件描述符为disk_manager_->open_file的返回值

   public:
    // This function is called before every test.
    void SetUp() override {
        ::testing::Test::SetUp();
        // For each test, we create a new DiskManager
        disk_manager_ = std::make_unique<DiskManager>();
        // 如果测试目录不存在，则先创建测试目录
        if (!disk_manager_->is_dir(TEST_DB_NAME)) {
            disk_manager_->create_dir(TEST_DB_NAME);
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
        // 进入测试目录
        if (chdir(TEST_DB_NAME.c_str()) < 0) {
            throw UnixError();
        }
        // 如果测试文件存在，则先删除原文件（最后留下来的文件存的是最后一个测试点的数据）
        if (disk_manager_->is_file(TEST_FILE_NAME_CCUR)) {
            disk_manager_->destroy_file(TEST_FILE_NAME_CCUR);
        }
        // 创建测试文件
        disk_manager_->create_file(TEST_FILE_NAME_CCUR);
        assert(disk_manager_->is_file(TEST_FILE_NAME_CCUR));
        // 打开测试文件
        fd_ = disk_manager_->open_file(TEST_FILE_NAME_CCUR);
        assert(fd_ != -1);
    }

    // This function is called after every test.
    void TearDown() override {
        disk_manager_->close_file(fd_);
        // disk_manager_->destroy_file(TEST_FILE_NAME_CCUR);  // you can choose to delete the file

        // 返回上一层目录
        if (chdir("..") < 0) {
            throw UnixError();
        }
        assert(disk_manager_->is_dir(TEST_DB_NAME));
    };
};

TEST_F(BufferPoolManagerConcurrencyTest, ConcurrencyTest) {
    const int num_threads = 5;
    const int num_runs = 50;

    // get fd
    int fd = BufferPoolManagerConcurrencyTest::fd_;

    for (int run = 0; run < num_runs; run++) {
        // create BufferPoolManager
        auto disk_manager = BufferPoolManagerConcurrencyTest::disk_manager_.get();
        std::shared_ptr<BufferPoolManager> bpm{new BufferPoolManager(50, disk_manager)};

        std::vector<std::thread> threads;
        for (int tid = 0; tid < num_threads; tid++) {
            threads.push_back(std::thread([&bpm, fd]() {  // NOLINT
                PageId temp_page_id = {.fd = fd, .page_no = INVALID_PAGE_ID};
                std::vector<PageId> page_ids;
                for (int i = 0; i < 10; i++) {
                    auto new_page = bpm->new_page(&temp_page_id);
                    EXPECT_NE(nullptr, new_page);
                    ASSERT_NE(nullptr, new_page);
                    strcpy(new_page->get_data(), std::to_string(temp_page_id.page_no).c_str());  // NOLINT
                    new_page->mark_wal_free_dirty();
                    page_ids.push_back(temp_page_id);
                }
                for (int i = 0; i < 10; i++) {
                    EXPECT_EQ(1, bpm->unpin_page(page_ids[i], true));
                }
                for (int j = 0; j < 10; j++) {
                    auto page = bpm->fetch_page(page_ids[j]);
                    EXPECT_NE(nullptr, page);
                    ASSERT_NE(nullptr, page);
                    EXPECT_EQ(0, std::strcmp(std::to_string(page_ids[j].page_no).c_str(), (page->get_data())));
                    // 本轮只校验页面内容，没有修改页面，不能把已写回的干净页重新标脏。
                    EXPECT_EQ(1, bpm->unpin_page(page_ids[j], false));
                }
                for (int j = 0; j < 10; j++) {
                    EXPECT_EQ(1, bpm->delete_page(page_ids[j]));
                }
                bpm->flush_all_pages(fd);  // add this test by jiawen
            }));
        }  // end loop tid=[0,num_threads)

        for (int i = 0; i < num_threads; i++) {
            threads[i].join();
        }
    }  // end loop run=[0,num_runs)
}

TEST_F(BufferPoolManagerConcurrencyTest, ConcurrentSamePageMissTest) {
    int fd = BufferPoolManagerConcurrencyTest::fd_;
    auto disk_manager = BufferPoolManagerConcurrencyTest::disk_manager_.get();
    auto bpm = std::make_shared<BufferPoolManager>(1, disk_manager);

    std::vector<char> page0(PAGE_SIZE, 0);
    std::vector<char> page1(PAGE_SIZE, 0);
    page0[0] = 'A';
    page0[1] = '0';
    page1[0] = 'B';
    page1[1] = '1';
    disk_manager->write_page(fd, 0, page0.data(), PAGE_SIZE);
    disk_manager->write_page(fd, 1, page1.data(), PAGE_SIZE);
    disk_manager->set_fd2pageno(fd, 2);

    const int kThreads = 8;
    const int kRounds = 200;
    for (int round = 0; round < kRounds; round++) {
        Page *warm = bpm->fetch_page(PageId{fd, 1});
        ASSERT_NE(warm, nullptr);
        EXPECT_EQ('B', warm->get_data()[0]);
        EXPECT_TRUE(bpm->unpin_page(PageId{fd, 1}, false));

        std::mutex start_mu;
        std::condition_variable start_cv;
        int ready = 0;
        bool go = false;
        std::atomic<int> ok_cnt{0};
        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int i = 0; i < kThreads; i++) {
            threads.emplace_back([&]() {
                {
                    std::unique_lock<std::mutex> lk(start_mu);
                    ready++;
                    if (ready == kThreads) {
                        start_cv.notify_all();
                    }
                    start_cv.wait(lk, [&]() { return go; });
                }
                Page *p = bpm->fetch_page(PageId{fd, 0});
                ASSERT_NE(p, nullptr);
                EXPECT_EQ('A', p->get_data()[0]);
                EXPECT_EQ('0', p->get_data()[1]);
                ok_cnt.fetch_add(1, std::memory_order_relaxed);
                EXPECT_TRUE(bpm->unpin_page(PageId{fd, 0}, false));
            });
        }

        {
            std::unique_lock<std::mutex> lk(start_mu);
            start_cv.wait(lk, [&]() { return ready == kThreads; });
            go = true;
            start_cv.notify_all();
        }

        for (auto &t : threads) {
            t.join();
        }
        EXPECT_EQ(ok_cnt.load(), kThreads);

        int frame_with_page0 = 0;
        {
            std::scoped_lock lock{bpm->latch_};
            for (size_t i = 0; i < bpm->pool_size_; i++) {
                if (bpm->pages_[i].id_.fd == fd && bpm->pages_[i].id_.page_no == 0) {
                    frame_with_page0++;
                    EXPECT_FALSE(bpm->frame_io_in_progress_[i]);
                }
            }
            EXPECT_EQ(1, frame_with_page0);
        }
    }
}

// 脏 victim 在 WAL/pwrite 完成前必须保持可等待状态。否则并发 miss
// 可能从磁盘读回旧版本，并把旧内容作为 clean 页长期留在缓冲池中。
TEST_F(BufferPoolManagerConcurrencyTest, DirtyEvictionBlocksStaleRefetch) {
    int fd = BufferPoolManagerConcurrencyTest::fd_;
    auto disk_manager = BufferPoolManagerConcurrencyTest::disk_manager_.get();
    auto bpm = std::make_shared<BufferPoolManager>(2, disk_manager);

    std::vector<char> disk_page(PAGE_SIZE, 0);
    strcpy(disk_page.data() + 32, "old-on-disk");
    disk_manager->write_page(fd, 0, disk_page.data(), PAGE_SIZE);
    disk_page.assign(PAGE_SIZE, 0);
    strcpy(disk_page.data() + 32, "load-target");
    disk_manager->write_page(fd, 1, disk_page.data(), PAGE_SIZE);
    disk_page.assign(PAGE_SIZE, 0);
    strcpy(disk_page.data() + 32, "second-frame");
    disk_manager->write_page(fd, 2, disk_page.data(), PAGE_SIZE);
    disk_manager->set_fd2pageno(fd, 3);

    Page *victim = bpm->fetch_page(PageId{fd, 0});
    ASSERT_NE(victim, nullptr);
    {
        PageWriteGuard guard(victim);
        strcpy(victim->get_data() + 32, "latest-dirty-version");
        victim->set_page_lsn(7);
    }
    ASSERT_TRUE(bpm->unpin_page(PageId{fd, 0}, true));

    Page *second = bpm->fetch_page(PageId{fd, 2});
    ASSERT_NE(second, nullptr);
    {
        PageWriteGuard guard(second);
        second->set_page_lsn(8);
    }
    ASSERT_TRUE(bpm->unpin_page(PageId{fd, 2}, true));

    std::mutex barrier_mutex;
    std::condition_variable barrier_cv;
    bool barrier_entered = false;
    bool release_barrier = false;
    bpm->set_wal_barrier(
        [](lsn_t) { return true; },
        [&](lsn_t) {
            std::unique_lock<std::mutex> lock(barrier_mutex);
            barrier_entered = true;
            barrier_cv.notify_all();
            barrier_cv.wait(lock, [&]() { return release_barrier; });
        });

    std::thread evictor([&]() {
        Page *target = bpm->fetch_page(PageId{fd, 1});
        ASSERT_NE(target, nullptr);
        EXPECT_STREQ(target->get_data() + 32, "load-target");
        EXPECT_TRUE(bpm->unpin_page(PageId{fd, 1}, false));
    });
    bool barrier_seen = false;
    {
        std::unique_lock<std::mutex> lock(barrier_mutex);
        barrier_seen = barrier_cv.wait_for(lock, std::chrono::seconds(2), [&]() { return barrier_entered; });
    }
    if (!barrier_seen) {
        {
            std::lock_guard<std::mutex> lock(barrier_mutex);
            release_barrier = true;
        }
        barrier_cv.notify_all();
        evictor.join();
        FAIL() << "dirty eviction did not reach WAL barrier";
    }

    std::mutex reader_mutex;
    std::condition_variable reader_cv;
    bool reader_done = false;
    bool reader_saw_latest = false;
    std::thread reader([&]() {
        Page *refetched = bpm->fetch_page(PageId{fd, 0});
        if (refetched != nullptr) {
            reader_saw_latest = strcmp(refetched->get_data() + 32, "latest-dirty-version") == 0;
            bpm->unpin_page(PageId{fd, 0}, false);
        }
        {
            std::lock_guard<std::mutex> lock(reader_mutex);
            reader_done = true;
        }
        reader_cv.notify_all();
    });

    {
        std::unique_lock<std::mutex> lock(reader_mutex);
        EXPECT_FALSE(reader_cv.wait_for(lock, std::chrono::milliseconds(100), [&]() { return reader_done; }));
    }
    {
        std::lock_guard<std::mutex> lock(barrier_mutex);
        release_barrier = true;
    }
    barrier_cv.notify_all();
    evictor.join();
    reader.join();

    EXPECT_TRUE(reader_done);
    EXPECT_TRUE(reader_saw_latest);
}

// 后台写回在 WAL barrier/pwrite 期间不得占用 BufferPool 分区锁；
// 若快照之后同页再次被修改，旧快照完成后也不得清除新 dirty。
TEST_F(BufferPoolManagerConcurrencyTest, WritebackDoesNotLoseConcurrentRedirty) {
    int fd = BufferPoolManagerConcurrencyTest::fd_;
    auto disk_manager = BufferPoolManagerConcurrencyTest::disk_manager_.get();
    auto bpm = std::make_shared<BufferPoolManager>(8, disk_manager);
    bpm->set_wal_barrier([](lsn_t) { return true; }, [](lsn_t) {});

    PageId page_id{fd, INVALID_PAGE_ID};
    Page *page = bpm->new_page(&page_id);
    ASSERT_NE(page, nullptr);
    {
        PageWriteGuard guard(page);
        strcpy(page->get_data() + 32, "base");
        page->set_page_lsn(1);
    }
    ASSERT_TRUE(bpm->unpin_page(page_id, true));
    ASSERT_TRUE(bpm->flush_page(page_id));
    ASSERT_EQ(bpm->dirty_page_count(), size_t(0));

    page = bpm->fetch_page(page_id);
    ASSERT_NE(page, nullptr);
    {
        PageWriteGuard guard(page);
        strcpy(page->get_data() + 32, "snapshot-before-writeback");
        page->set_page_lsn(2);
    }
    ASSERT_TRUE(bpm->unpin_page(page_id, true));

    std::mutex barrier_mutex;
    std::condition_variable barrier_cv;
    bool barrier_entered = false;
    bool release_barrier = false;
    bpm->set_wal_barrier(
        [](lsn_t) { return true; },
        [&](lsn_t) {
            std::unique_lock<std::mutex> lock(barrier_mutex);
            if (!barrier_entered) {
                barrier_entered = true;
                barrier_cv.notify_all();
                barrier_cv.wait(lock, [&]() { return release_barrier; });
            }
        });

    size_t flushed = 0;
    std::thread flusher([&]() { flushed = bpm->flush_some_dirty_pages(1, 0); });
    bool barrier_seen = false;
    {
        std::unique_lock<std::mutex> lock(barrier_mutex);
        barrier_seen = barrier_cv.wait_for(lock, std::chrono::seconds(2), [&]() { return barrier_entered; });
    }
    if (!barrier_seen) {
        {
            std::lock_guard<std::mutex> lock(barrier_mutex);
            release_barrier = true;
        }
        barrier_cv.notify_all();
        flusher.join();
        FAIL() << "writeback did not reach WAL barrier";
    }

    std::mutex writer_mutex;
    std::condition_variable writer_cv;
    bool writer_done = false;
    std::thread writer([&]() {
        Page *current = bpm->fetch_page(page_id);
        if (current != nullptr) {
            {
                PageWriteGuard guard(current);
                strcpy(current->get_data() + 32, "newer-concurrent-version");
                current->set_page_lsn(3);
            }
            bpm->unpin_page(page_id, true);
        }
        {
            std::lock_guard<std::mutex> lock(writer_mutex);
            writer_done = current != nullptr;
        }
        writer_cv.notify_all();
    });

    bool writer_completed_while_io_waited = false;
    {
        std::unique_lock<std::mutex> lock(writer_mutex);
        writer_completed_while_io_waited =
            writer_cv.wait_for(lock, std::chrono::seconds(2), [&]() { return writer_done; });
    }
    {
        std::lock_guard<std::mutex> lock(barrier_mutex);
        release_barrier = true;
    }
    barrier_cv.notify_all();
    writer.join();
    flusher.join();

    ASSERT_TRUE(writer_completed_while_io_waited);
    ASSERT_EQ(flushed, size_t(1));
    EXPECT_EQ(bpm->dirty_page_count(), size_t(1));

    ASSERT_TRUE(bpm->flush_page(page_id));
    EXPECT_EQ(bpm->dirty_page_count(), size_t(0));
    std::vector<char> disk_page(PAGE_SIZE, 0);
    disk_manager->read_page(fd, page_id.page_no, disk_page.data(), PAGE_SIZE);
    EXPECT_STREQ(disk_page.data() + 32, "newer-concurrent-version");
}

TEST_F(BufferPoolManagerConcurrencyTest, CleanerHonorsWatermarkAndWalBarrier) {
    int fd = BufferPoolManagerConcurrencyTest::fd_;
    auto disk_manager = BufferPoolManagerConcurrencyTest::disk_manager_.get();
    auto bpm = std::make_shared<BufferPoolManager>(32, disk_manager);

    PageId guarded_page{fd, INVALID_PAGE_ID};
    Page *page = bpm->new_page(&guarded_page);
    ASSERT_NE(page, nullptr);
    {
        PageWriteGuard guard(page);
        strcpy(page->get_data() + 32, "wal-guarded");
        page->set_page_lsn(11);
    }
    ASSERT_TRUE(bpm->unpin_page(guarded_page, true));
    bpm->set_wal_barrier([](lsn_t) { return false; }, [](lsn_t) {});
    EXPECT_THROW(bpm->flush_page(guarded_page), InternalError);
    EXPECT_EQ(bpm->dirty_page_count(), size_t(1));

    bpm->set_wal_barrier([](lsn_t) { return true; }, [](lsn_t) {});
    for (int i = 0; i < 11; ++i) {
        PageId page_id{fd, INVALID_PAGE_ID};
        Page *dirty = bpm->new_page(&page_id);
        ASSERT_NE(dirty, nullptr);
        {
            PageWriteGuard guard(dirty);
            snprintf(dirty->get_data() + 32, 32, "dirty-%d", i);
            dirty->mark_wal_free_dirty();
        }
        ASSERT_TRUE(bpm->unpin_page(page_id, true));
    }
    ASSERT_EQ(bpm->dirty_page_count(), size_t(12));

    bpm->StartPageCleaner(4, 4);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (bpm->dirty_page_count() > 4 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    bpm->StopPageCleaner();
    EXPECT_LE(bpm->dirty_page_count(), size_t(4));
}

TEST_F(BufferPoolManagerConcurrencyTest, ResidentPinAllowsCleanWritebackAndDirectUnpin) {
    int fd = BufferPoolManagerConcurrencyTest::fd_;
    auto disk_manager = BufferPoolManagerConcurrencyTest::disk_manager_.get();
    auto bpm = std::make_shared<BufferPoolManager>(8, disk_manager);

    PageId page_id{fd, INVALID_PAGE_ID};
    Page *created = bpm->new_page(&page_id);
    ASSERT_NE(created, nullptr);
    {
        PageWriteGuard guard(created);
        strcpy(created->get_data() + 32, "resident-root");
        created->mark_wal_free_dirty();
    }
    ASSERT_TRUE(bpm->unpin_page(created, true));

    Page *resident = bpm->fetch_resident_page(page_id);
    ASSERT_NE(resident, nullptr);
    EXPECT_FALSE(bpm->unpin_page(resident, false));
    EXPECT_FALSE(bpm->delete_page(page_id));

    // resident pin 只负责防淘汰，不应让 writeback 把没有普通访问者的页永久留脏。
    ASSERT_TRUE(bpm->flush_page(page_id));
    EXPECT_FALSE(resident->is_dirty());
    EXPECT_EQ(bpm->dirty_page_count(), size_t(0));

    Page *ordinary = bpm->fetch_page(page_id);
    ASSERT_EQ(ordinary, resident);
    EXPECT_STREQ(ordinary->get_data() + 32, "resident-root");
    EXPECT_TRUE(bpm->unpin_page(ordinary, false));

    ASSERT_TRUE(bpm->release_resident_page(resident));
    EXPECT_TRUE(bpm->delete_page(page_id));
}

TEST_F(BufferPoolManagerConcurrencyTest, DiscardAllPagesPreventsFdReuseAlias) {
    auto disk_manager = BufferPoolManagerConcurrencyTest::disk_manager_.get();
    auto bpm = std::make_shared<BufferPoolManager>(8, disk_manager);

    PageId old_id{fd_, INVALID_PAGE_ID};
    Page *old_page = bpm->new_page(&old_id);
    ASSERT_NE(old_page, nullptr);
    {
        PageWriteGuard guard(old_page);
        strcpy(old_page->get_data() + 32, "old-file-page");
        old_page->mark_wal_free_dirty();
    }
    ASSERT_TRUE(bpm->unpin_page(old_page, true));
    bpm->flush_all_pages(fd_);
    ASSERT_TRUE(bpm->discard_all_pages(fd_));

    auto &part = bpm->partitions_[bpm->partition_of(old_id)];
    std::scoped_lock lock{part.latch};
    EXPECT_EQ(part.page_table.count(old_id), size_t{0});
}

TEST_F(BufferPoolManagerConcurrencyTest, DiscardAllPagesPreflightKeepsCacheOnPinnedFailure) {
    auto disk_manager = BufferPoolManagerConcurrencyTest::disk_manager_.get();
    auto bpm = std::make_shared<BufferPoolManager>(8, disk_manager);

    PageId first_id{fd_, INVALID_PAGE_ID};
    PageId pinned_id{fd_, INVALID_PAGE_ID};
    Page *first_page = bpm->new_page(&first_id);
    Page *pinned_page = bpm->new_page(&pinned_id);
    ASSERT_NE(first_page, nullptr);
    ASSERT_NE(pinned_page, nullptr);
    ASSERT_TRUE(bpm->unpin_page(first_page, false));

    // 任一页仍被使用时不得开始清理，失败后先前的无 pin 页仍应留在缓存中。
    EXPECT_FALSE(bpm->discard_all_pages(fd_));
    auto &first_part = bpm->partitions_[bpm->partition_of(first_id)];
    {
        std::scoped_lock lock{first_part.latch};
        EXPECT_EQ(first_part.page_table.count(first_id), size_t{1});
    }

    ASSERT_TRUE(bpm->unpin_page(pinned_page, false));
    EXPECT_TRUE(bpm->discard_all_pages(fd_));
}

TEST(PageWalMetadataTest, SidecarDoesNotOverwriteIndexFreeChainBytes) {
    Page page;
    page_id_t next_free_page_no = 15;
    memcpy(page.get_data(), &next_free_page_no, sizeof(next_free_page_no));

    page.set_page_lsn(1234);
    page_id_t stored = -999;
    memcpy(&stored, page.get_data(), sizeof(stored));
    EXPECT_EQ(stored, next_free_page_no);
    EXPECT_EQ(page.get_page_lsn(), 1234);
    EXPECT_EQ(page.get_wal_dirty_state(), Page::WalDirtyState::kWalBacked);

    page.reset_memory();
    EXPECT_EQ(page.get_page_lsn(), INVALID_LSN);
    EXPECT_EQ(page.get_wal_dirty_state(), Page::WalDirtyState::kClean);
}

TEST_F(BufferPoolManagerConcurrencyTest, AdaptiveCleanerUsesPartitionPressureBelowGlobalWatermark) {
    int fd = BufferPoolManagerConcurrencyTest::fd_;
    auto disk_manager = BufferPoolManagerConcurrencyTest::disk_manager_.get();
    auto bpm = std::make_shared<BufferPoolManager>(32, disk_manager);

    std::vector<char> disk_page(PAGE_SIZE, 0);
    // 4 个分区、每分区 8 帧；构造同一分区 7 个脏页，超过该分区 75%
    // 高水位，但仍远低于全局阈值 31。
    for (int page_no : {0, 4, 8, 12, 16, 20, 24}) {
        disk_manager->write_page(fd, page_no, disk_page.data(), PAGE_SIZE);
        Page *page = bpm->fetch_page(PageId{fd, page_no});
        ASSERT_NE(page, nullptr);
        {
            PageWriteGuard guard(page);
            page->mark_wal_free_dirty();
            memcpy(page->get_data() + 32, &page_no, sizeof(page_no));
        }
        ASSERT_TRUE(bpm->unpin_page(PageId{fd, page_no}, true));
    }
    ASSERT_EQ(bpm->dirty_page_count(), size_t(7));
    ASSERT_LT(bpm->dirty_page_count(), size_t(31));
    ASSERT_EQ(bpm->partition_of(PageId{fd, 0}), bpm->partition_of(PageId{fd, 4}));

    bpm->StartPageCleaner(2, 31, true);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (bpm->dirty_page_count() > 4 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    bpm->StopPageCleaner();
    EXPECT_LE(bpm->dirty_page_count(), size_t(4));
}

TEST_F(BufferPoolManagerConcurrencyTest, AdaptiveCleanerProtectsRealCleanVictimReserve) {
    int fd = BufferPoolManagerConcurrencyTest::fd_;
    auto disk_manager = BufferPoolManagerConcurrencyTest::disk_manager_.get();
    auto bpm = std::make_shared<BufferPoolManager>(8, disk_manager);
    std::vector<char> disk_page(PAGE_SIZE, 0);
    for (int page_no = 0; page_no < 8; ++page_no) {
        disk_manager->write_page(fd, page_no, disk_page.data(), PAGE_SIZE);
    }

    Page *dirty = bpm->fetch_page(PageId{fd, 0});
    ASSERT_NE(dirty, nullptr);
    {
        PageWriteGuard guard(dirty);
        dirty->mark_wal_free_dirty();
        strcpy(dirty->get_data() + 32, "reserve-source");
    }
    ASSERT_TRUE(bpm->unpin_page(PageId{fd, 0}, true));

    std::vector<PageId> pinned;
    for (int page_no = 1; page_no < 8; ++page_no) {
        PageId page_id{fd, page_no};
        ASSERT_NE(bpm->fetch_page(page_id), nullptr);
        pinned.push_back(page_id);
    }
    ASSERT_EQ(bpm->partitions_[0].clean_available_count.load(), size_t(0));
    ASSERT_EQ(bpm->dirty_page_count(), size_t(1));

    bpm->StartPageCleaner(2, 7, true);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (bpm->dirty_page_count() != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    bpm->StopPageCleaner();
    EXPECT_EQ(bpm->dirty_page_count(), size_t(0));
    for (const auto &page_id : pinned) {
        EXPECT_TRUE(bpm->unpin_page(page_id, false));
    }
}

TEST_F(BufferPoolManagerConcurrencyTest, AfterWalFastPathStillRejectsUndeclaredDirtyPage) {
    int fd = BufferPoolManagerConcurrencyTest::fd_;
    auto disk_manager = BufferPoolManagerConcurrencyTest::disk_manager_.get();
    auto bpm = std::make_shared<BufferPoolManager>(2, disk_manager);

    PageId page_id{fd, INVALID_PAGE_ID};
    Page *page = bpm->new_page(&page_id);
    ASSERT_NE(page, nullptr);
    page->mark_wal_free_dirty();
    ASSERT_TRUE(bpm->unpin_page(page_id, true));
    ASSERT_TRUE(bpm->flush_page(page_id));

    page = bpm->fetch_page(page_id);
    ASSERT_NE(page, nullptr);
    strcpy(page->get_data() + 32, "missing-provenance");
    ASSERT_TRUE(bpm->unpin_page(page_id, true));
    page = bpm->fetch_page(page_id);
    ASSERT_NE(page, nullptr);
    page->set_page_lsn(99);
    page->mark_wal_free_dirty();
    ASSERT_TRUE(bpm->unpin_page(page_id, false));
    EXPECT_THROW(bpm->flush_some_dirty_pages_after_wal(1, 0), InternalError);
    EXPECT_EQ(bpm->dirty_page_count(), size_t(1));
}

TEST_F(BufferPoolManagerConcurrencyTest, FrameReuseClearsWalSidecar) {
    int fd = BufferPoolManagerConcurrencyTest::fd_;
    auto disk_manager = BufferPoolManagerConcurrencyTest::disk_manager_.get();
    auto bpm = std::make_shared<BufferPoolManager>(1, disk_manager);
    bpm->set_wal_barrier([](lsn_t) { return true; }, [](lsn_t) {});

    PageId first_id{fd, INVALID_PAGE_ID};
    Page *first = bpm->new_page(&first_id);
    ASSERT_NE(first, nullptr);
    {
        PageWriteGuard guard(first);
        first->set_page_lsn(42);
        strcpy(first->get_data() + 32, "old-frame");
    }
    ASSERT_TRUE(bpm->unpin_page(first_id, true));

    PageId target{fd, first_id.page_no + 1};
    std::vector<char> disk_page(PAGE_SIZE, 0);
    strcpy(disk_page.data() + 32, "new-frame");
    disk_manager->write_page(fd, target.page_no, disk_page.data(), PAGE_SIZE);
    Page *loaded = bpm->fetch_page(target);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->get_page_lsn(), INVALID_LSN);
    EXPECT_EQ(loaded->get_wal_dirty_state(), Page::WalDirtyState::kClean);
    EXPECT_STREQ(loaded->get_data() + 32, "new-frame");
    EXPECT_TRUE(bpm->unpin_page(target, false));
}

TEST_F(BufferPoolManagerConcurrencyTest, BatchWalValidationRejectsAnyUnknownPageLsn) {
    int fd = BufferPoolManagerConcurrencyTest::fd_;
    auto disk_manager = BufferPoolManagerConcurrencyTest::disk_manager_.get();
    auto bpm = std::make_shared<BufferPoolManager>(8, disk_manager);
    std::atomic<int> flush_calls{0};
    bpm->set_wal_barrier([](lsn_t lsn) { return lsn != 15; },
                         [&](lsn_t) { flush_calls.fetch_add(1); });

    for (lsn_t lsn : {15, 200}) {
        PageId page_id{fd, INVALID_PAGE_ID};
        Page *page = bpm->new_page(&page_id);
        ASSERT_NE(page, nullptr);
        {
            PageWriteGuard guard(page);
            page->set_page_lsn(lsn);
        }
        ASSERT_TRUE(bpm->unpin_page(page_id, true));
    }

    EXPECT_THROW(bpm->flush_some_dirty_pages(2, 0), InternalError);
    EXPECT_EQ(flush_calls.load(), 0);
    EXPECT_EQ(bpm->dirty_page_count(), size_t(2));
}

TEST_F(BufferPoolManagerConcurrencyTest, ForegroundPrefersBoundedCleanVictim) {
    int fd = BufferPoolManagerConcurrencyTest::fd_;
    auto disk_manager = BufferPoolManagerConcurrencyTest::disk_manager_.get();
    auto bpm = std::make_shared<BufferPoolManager>(2, disk_manager);
    std::vector<char> disk_page(PAGE_SIZE, 0);
    for (int page_no = 0; page_no < 3; ++page_no) {
        disk_manager->write_page(fd, page_no, disk_page.data(), PAGE_SIZE);
    }

    Page *dirty = bpm->fetch_page(PageId{fd, 0});
    ASSERT_NE(dirty, nullptr);
    {
        PageWriteGuard guard(dirty);
        dirty->mark_wal_free_dirty();
        strcpy(dirty->get_data() + 32, "keep-dirty-resident");
    }
    ASSERT_TRUE(bpm->unpin_page(PageId{fd, 0}, true));
    Page *clean = bpm->fetch_page(PageId{fd, 2});
    ASSERT_NE(clean, nullptr);
    ASSERT_TRUE(bpm->unpin_page(PageId{fd, 2}, false));

    Page *target = bpm->fetch_page(PageId{fd, 1});
    ASSERT_NE(target, nullptr);
    auto &part = bpm->partitions_[bpm->partition_of(PageId{fd, 0})];
    EXPECT_TRUE(part.page_table.count(PageId{fd, 0}) > 0);
    EXPECT_EQ(part.page_table.count(PageId{fd, 2}), size_t(0));
    EXPECT_EQ(bpm->sync_dirty_evictions_.load(), std::uint64_t{0});
    EXPECT_TRUE(bpm->unpin_page(PageId{fd, 1}, false));
}

TEST_F(BufferPoolManagerConcurrencyTest, CleanerKeepsCleanedColdPageAtLruTail) {
    int fd = BufferPoolManagerConcurrencyTest::fd_;
    auto disk_manager = BufferPoolManagerConcurrencyTest::disk_manager_.get();
    auto bpm = std::make_shared<BufferPoolManager>(2048, disk_manager);
    std::vector<char> disk_page(PAGE_SIZE, 0);

    // 2048 帧退化为 16 个分区，每分区 128 帧。填满同一分区并全部标脏，
    // 让 cleaner 清出的唯一 clean 页与其它 dirty 页相隔超过 64 个位置。
    for (int i = 0; i < 128; ++i) {
        PageId page_id{fd, i * 16};
        disk_manager->write_page(fd, page_id.page_no, disk_page.data(), PAGE_SIZE);
        Page *page = bpm->fetch_page(page_id);
        ASSERT_NE(page, nullptr);
        {
            PageWriteGuard guard(page);
            page->mark_wal_free_dirty();
            memcpy(page->get_data() + 32, &i, sizeof(i));
        }
        ASSERT_TRUE(bpm->unpin_page(page_id, true));
    }
    ASSERT_EQ(bpm->flush_some_dirty_pages(1, 0), size_t(1));

    PageId target{fd, 128 * 16};
    disk_manager->write_page(fd, target.page_no, disk_page.data(), PAGE_SIZE);
    Page *loaded = bpm->fetch_page(target);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(bpm->sync_dirty_evictions_.load(), std::uint64_t{0});
    EXPECT_TRUE(bpm->unpin_page(target, false));
}

TEST_F(BufferPoolManagerConcurrencyTest, BatchPartialWriteFailureReleasesEveryReservation) {
    auto disk_manager = std::make_unique<DiskManager>();
    const std::string first_name = "writeback_partial_first";
    const std::string second_name = "writeback_partial_second";
    for (const auto &name : {first_name, second_name}) {
        if (disk_manager->is_file(name)) {
            disk_manager->destroy_file(name);
        }
        disk_manager->create_file(name);
    }
    int first_fd = disk_manager->open_file(first_name);
    int second_fd = disk_manager->open_file(second_name);
    auto bpm = std::make_unique<BufferPoolManager>(16, disk_manager.get());

    for (int fd : {first_fd, second_fd}) {
        PageId page_id{fd, INVALID_PAGE_ID};
        Page *page = bpm->new_page(&page_id);
        ASSERT_NE(page, nullptr);
        page->mark_wal_free_dirty();
        ASSERT_TRUE(bpm->unpin_page(page_id, true));
    }

    // fd 较小的第一页先成功，随后关闭第二个 fd，稳定制造批内第 2 次
    // pwrite 失败。失败页及尚未写入页都必须释放临时 pin/writeback 标志。
    ASSERT_EQ(::close(second_fd), 0);
    EXPECT_THROW(bpm->flush_some_dirty_pages(2, 0), InternalError);
    auto &part = bpm->partitions_[bpm->partition_of(PageId{second_fd, 0})];
    auto it = part.page_table.find(PageId{second_fd, 0});
    ASSERT_NE(it, part.page_table.end());
    frame_id_t failed_frame = it->second;
    EXPECT_EQ(bpm->pages_[failed_frame].pin_count_, 0);
    EXPECT_TRUE(bpm->pages_[failed_frame].is_dirty_);
    EXPECT_FALSE(bpm->frame_writeback_in_progress_[failed_frame]);
    EXPECT_FALSE(bpm->frame_writeback_snapshot_ready_[failed_frame]);

    bpm.reset();
    disk_manager->close_file(first_fd);
    try {
        disk_manager->close_file(second_fd);
    } catch (const UnixError &) {
        // 映射已由 close_file 移除；底层 fd 是本测试为注入失败而提前关闭。
    }
    disk_manager->destroy_file(first_name);
    disk_manager->destroy_file(second_name);
}

// TODO: fix detected memory leaks found by Google Test
TEST(StorageTest, SimpleTest) {
    srand((unsigned)time(nullptr));

    /** Test disk_manager */
    std::vector<std::string> filenames(MAX_FILES);  // MAX_FILES=32
    std::unordered_map<int, std::string> fd2name;
    for (size_t i = 0; i < filenames.size(); i++) {
        auto &filename = filenames[i];
        filename = std::to_string(i) + ".txt";
        if (disk_manager->is_file(filename)) {
            disk_manager->destroy_file(filename);
        }
        // open without create
        try {
            disk_manager->open_file(filename);
            assert(false);
        } catch (const FileNotFoundError &e) {
        }

        disk_manager->create_file(filename);
        assert(disk_manager->is_file(filename));
        try {
            disk_manager->create_file(filename);
            assert(false);
        } catch (const FileExistsError &e) {
        }

        // open file
        int fd = disk_manager->open_file(filename);
        char *tmp = new char[PAGE_SIZE * MAX_PAGES];  // TODO: fix error in detected memory leaks

        mock[fd] = tmp;
        fd2name[fd] = filename;

        disk_manager->set_fd2pageno(fd, 0);  // diskmanager在fd对应的文件中从0开始分配page_no
    }

    /** Test buffer_pool_manager*/
    int num_pages = 0;
    char init_buf[PAGE_SIZE];
    for (auto &fh : mock) {
        int fd = fh.first;
        for (page_id_t i = 0; i < MAX_PAGES; i++) {
            rand_buf(PAGE_SIZE, init_buf);  // 将init_buf填充PAGE_SIZE个字节的随机数据

            PageId tmp_page_id = {.fd = fd, .page_no = INVALID_PAGE_ID};
            Page *page = buffer_pool_manager->new_page(&tmp_page_id);
            int page_no = tmp_page_id.page_no;
            assert(page_no != INVALID_PAGE_ID);
            assert(page_no == i);

            memcpy(page->get_data(), init_buf, PAGE_SIZE);
            page->mark_wal_free_dirty();
            buffer_pool_manager->unpin_page(PageId{fd, page_no}, true);

            char *mock_buf = mock_get_page(fd, page_no);  // &mock[fd][page_no * PAGE_SIZE]
            memcpy(mock_buf, init_buf, PAGE_SIZE);

            num_pages++;

            check_cache(fd, page_no);  // 调用了fetch_page, unpin_page
        }
    }
    check_cache_all();

    assert(num_pages == TEST_BUFFER_POOL_SIZE);

    /** Test flush_all_pages() */
    // Flush and test disk
    for (auto &entry : fd2name) {
        int fd = entry.first;
        buffer_pool_manager->flush_all_pages(fd);
        for (int page_no = 0; page_no < MAX_PAGES; page_no++) {
            check_disk(fd, page_no);
        }
    }
    check_disk_all();

    for (int r = 0; r < 10000; r++) {
        int fd = rand_fd();
        int page_no = rand() % MAX_PAGES;
        // fetch page
        Page *page = buffer_pool_manager->fetch_page(PageId{fd, page_no});
        char *mock_buf = mock_get_page(fd, page_no);
        assert(memcmp(page->get_data(), mock_buf, PAGE_SIZE) == 0);

        // modify
        rand_buf(PAGE_SIZE, init_buf);
        memcpy(page->get_data(), init_buf, PAGE_SIZE);
        memcpy(mock_buf, init_buf, PAGE_SIZE);
        // 该存储层随机写测试没有事务日志，显式声明为允许无 WAL 的脏页。
        page->mark_wal_free_dirty();

        buffer_pool_manager->unpin_page(page->get_page_id(), true);
        // BufferPool::mark_dirty(page);

        // flush
        if (rand() % 10 == 0) {
            buffer_pool_manager->flush_page(page->get_page_id());
            check_disk(fd, page_no);
        }
        // flush entire file
        if (rand() % 100 == 0) {
            buffer_pool_manager->flush_all_pages(fd);
        }
        // re-open file
        if (rand() % 100 == 0) {
            disk_manager->close_file(fd);
            auto filename = fd2name[fd];
            char *buf = mock[fd];
            fd2name.erase(fd);
            mock.erase(fd);
            int new_fd = disk_manager->open_file(filename);
            mock[new_fd] = buf;
            fd2name[new_fd] = filename;
        }
        // assert equal in cache
        check_cache(fd, page_no);
    }
    check_cache_all();

    for (auto &entry : fd2name) {
        int fd = entry.first;
        buffer_pool_manager->flush_all_pages(fd);
        for (int page_no = 0; page_no < MAX_PAGES; page_no++) {
            check_disk(fd, page_no);
        }
    }
    check_disk_all();

    // close and destroy files
    for (auto &entry : fd2name) {
        int fd = entry.first;
        auto &filename = entry.second;
        disk_manager->close_file(fd);
        disk_manager->destroy_file(filename);
        try {
            disk_manager->destroy_file(filename);
            assert(false);
        } catch (const FileNotFoundError &e) {
        }
    }
}

TEST(RecordManagerTest, SimpleTest) {
    srand((unsigned)time(nullptr));

    // 创建RmManager类的对象rm_manager
    auto disk_manager = std::make_unique<DiskManager>();
    // 记录管理语义与提交配置的 4GB 容量无关；单测使用独立小池，
    // 避免低内存开发机在进入真正的 RecordManager 断言前就被 OOM 终止。
    auto buffer_pool_manager = std::make_unique<BufferPoolManager>(TEST_BUFFER_POOL_SIZE, disk_manager.get());
    auto rm_manager = std::make_unique<RmManager>(disk_manager.get(), buffer_pool_manager.get());

    std::unordered_map<Rid, std::string, rid_hash_t, rid_equal_t> mock;

    std::string filename = "abc.txt";

    int record_size = 4 + rand() % 256;  // 元组大小随便设置，只要不超过RM_MAX_RECORD_SIZE
    // test files
    {
        // 删除残留的同名文件
        if (disk_manager->is_file(filename)) {
            disk_manager->destroy_file(filename);
        }
        // 将file header写入到磁盘中的filename文件
        rm_manager->create_file(filename, record_size);
        // 将磁盘中的filename文件读出到内存中的file handle的file header
        std::unique_ptr<RmFileHandle> file_handle = rm_manager->open_file(filename);
        // 检查filename文件在内存中的file header的参数
        assert(file_handle->file_hdr_.record_size == record_size);
        assert(file_handle->file_hdr_.first_free_page_no == RM_NO_PAGE);
        assert(file_handle->file_hdr_.num_pages == 1);

        int max_bytes = file_handle->file_hdr_.record_size * file_handle->file_hdr_.num_records_per_page +
                        file_handle->file_hdr_.bitmap_size + (int)sizeof(RmPageHdr);
        assert(max_bytes <= PAGE_SIZE);
        int rand_val = rand();
        file_handle->file_hdr_.num_pages = rand_val;
        rm_manager->close_file(file_handle.get());

        // reopen file
        file_handle = rm_manager->open_file(filename);
        assert(file_handle->file_hdr_.num_pages == rand_val);
        rm_manager->close_file(file_handle.get());
        rm_manager->destroy_file(filename);
    }
    // test pages
    rm_manager->create_file(filename, record_size);
    auto file_handle = rm_manager->open_file(filename);

    char write_buf[PAGE_SIZE];
    size_t add_cnt = 0;
    size_t upd_cnt = 0;
    size_t del_cnt = 0;
    for (int round = 0; round < 1000; round++) {
        double insert_prob = 1. - mock.size() / 250.;
        double dice = rand() * 1. / RAND_MAX;
        if (mock.empty() || dice < insert_prob) {
            rand_buf(file_handle->file_hdr_.record_size, write_buf);
            Rid rid = file_handle->insert_record(write_buf, nullptr);
            mock[rid] = std::string((char *)write_buf, file_handle->file_hdr_.record_size);
            add_cnt++;
            //            std::cout << "insert " << rid << '\n'; // operator<<(cout,rid)
        } else {
            // update or erase random rid
            int rid_idx = rand() % mock.size();
            auto it = mock.begin();
            for (int i = 0; i < rid_idx; i++) {
                it++;
            }
            auto rid = it->first;
            if (rand() % 2 == 0) {
                // update
                rand_buf(file_handle->file_hdr_.record_size, write_buf);
                file_handle->update_record(rid, write_buf, nullptr);
                mock[rid] = std::string((char *)write_buf, file_handle->file_hdr_.record_size);
                upd_cnt++;
                //                std::cout << "update " << rid << '\n';
            } else {
                // erase
                file_handle->delete_record(rid, nullptr);
                mock.erase(rid);
                del_cnt++;
                //                std::cout << "delete " << rid << '\n';
            }
        }
        // Randomly re-open file
        if (round % 50 == 0) {
            rm_manager->close_file(file_handle.get());
            file_handle = rm_manager->open_file(filename);
        }
        check_equal(file_handle.get(), mock);
    }
    assert(mock.size() == add_cnt - del_cnt);
    std::cout << "insert " << add_cnt << '\n' << "delete " << del_cnt << '\n' << "update " << upd_cnt << '\n';
    // clean up
    rm_manager->close_file(file_handle.get());
    rm_manager->destroy_file(filename);
}
