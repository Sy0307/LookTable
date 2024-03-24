/*
 * 设计一个性能优化的查找表looktable.
 * 问题：
 *    交易程序下单的时候，交易所会返回int64_t order_id, order_id在一天内是时间递增的， 大部分的order生命周期都是短暂，
 *  如果order生命结束，对它就失去兴趣了，可以从looktable删掉。
 * 要求：
 *  1. 只需要满足一天的需求，order_id范围100万级别。
 *  2. insert, erase不要频繁分配内存(比如malloc, new)
 *  3. 至少提供一个模板参数T, T可能保存order的相关信息。
 */

/*
目前我实现了一个无锁的looktable，
使用了三次hash，每次hash都是一个质数，
最后取模得到一个索引，然后使用原子操作插入数据。

在测试中，我测试了int和string两种类型的插入性能，
同时也测试了多线程并发插入和删除的安全，虽然因为时间有限，我并没有完整的测试所有的情况。
但是我保证基本的线程安全。不过仍然有一些情况等待改进。


*/

#define CATCH_CONFIG_MAIN
#include "catch.hpp"
#include <iostream>
#include <atomic>
#include <array>
#include <memory>
#include <optional>
#include <chrono>
#include <string>
#include <vector>
#include <thread>

const uint64_t MAX_SIZE = 1000000;

namespace Triple_Hash
{
    const uint64_t PRIME1 = 2654435761;
    const uint64_t PRIME2 = 2246822519;
    const uint64_t PRIME3 = 3266489917;
    const uint64_t MOD1 = 1e9 + 7;
    const uint64_t MOD2 = 1e9 + 9;
    const uint64_t MOD3 = MAX_SIZE;

    uint64_t get(uint64_t id)
    {
        uint64_t hash1 = (id * PRIME1) % MOD1;
        uint64_t hash2 = (hash1 * PRIME2) % MOD2;
        uint64_t hash3 = (hash2 * PRIME3) % MOD3;
        return hash3;
    }
}

template <typename T>
class LookTable
{
public:
    void insert(int64_t id, T &&t)
    {
        auto hash = Triple_Hash::get(id);
        // std::cout<<hash<<std::endl;
        auto newNode = std::make_unique<Node>(id, std::forward<T>(t));
        auto &head = table[hash];
        auto oldHead = head.load();
        newNode->next = oldHead;
        while (!head.compare_exchange_weak(oldHead, newNode.get()))
        {
            newNode->next = oldHead;
        }
        newNode.release();
        sizeCounter.fetch_add(1);
    }

    std::optional<T *> find(int64_t id)
    {
        auto hash = Triple_Hash::get(id);
        auto currentNode = table[hash].load();
        while (currentNode)
        {
            if (currentNode->id == id)
            {
                return &currentNode->data;
            }
            currentNode = currentNode->next;
        }
        return std::nullopt;
    }

    void erase(int64_t id)
    {
        auto hash = Triple_Hash::get(id);
        auto &head = table[hash];
        auto oldHead = head.load();
        while (oldHead)
        {
            if (oldHead->id == id)
            {
                auto next = oldHead->next;
                if (head.compare_exchange_weak(oldHead, next))
                {
                    delete oldHead;
                    return;
                }
            }
            oldHead = oldHead->next;
        }
        sizeCounter.fetch_sub(1);
    }

    uint64_t size()
    {
        return sizeCounter.load();
    }

private:
    struct Node
    {
        int64_t id;
        T data;
        Node *next;
        Node(int64_t id, T &&t) : id(id), data(std::forward<T>(t)), next(nullptr) {}
    };
    std::atomic<uint64_t> sizeCounter{0};
    std::array<std::atomic<Node *>, MAX_SIZE> table;
};

TEST_CASE("LookTable operations")
{

    LookTable<int> lookTable;
    SECTION("Insert and find")
    {
        int64_t orderId = 123456;
        int orderValue = 42;
        lookTable.insert(orderId, std::move(orderValue));

        auto found = lookTable.find(orderId);
        REQUIRE((*found.value()) == orderValue);
        // lookTable.clear();
        lookTable.erase(orderId);
    }

    SECTION("Erase")
    {
        lookTable.insert(100, 42);
        lookTable.insert(200, 43);

        lookTable.erase(100);
        REQUIRE_FALSE(lookTable.find(100).has_value());
        REQUIRE(*(lookTable.find(200).value()) == 43);

        lookTable.erase(200);
    }

    SECTION("Performance for int")
    {
        constexpr int NUM_INSERTS = 5000000;

        auto start = std::chrono::steady_clock::now();

        for (int i = 0; i < NUM_INSERTS; i++)
        {
            lookTable.insert(i, std::move(i));
        }

        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        double average_time = duration * 1000000.0 / NUM_INSERTS;
        std::cout << "Total insert time: " << duration << " ms" << std::endl;
        std::cout << "Average insert time: " << average_time << " ns" << std::endl;
    }
}

TEST_CASE("SRTING_INSERT")
{
    SECTION("Performance for string")
    {

        LookTable<std::string> lookTable_string;
        constexpr int NUM_INSERTS = 5000000;
        constexpr int STRING_SIZE = 10000;
        std::vector<std::string> strings;
        for (int i = 0; i < NUM_INSERTS; i++)
        {
            strings.push_back(std::string(STRING_SIZE, 'a'));
        }

        auto start = std::chrono::steady_clock::now();

        for (int i = 0; i < NUM_INSERTS; i++)
        {
            // auto str = std::string(STRING_SIZE, 'a');
            // 主要性能瓶颈在于string的构造
            lookTable_string.insert(i, std::move(strings[i]));
        }

        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        double average_time = duration * 1000000.0 / NUM_INSERTS;
        std::cout << "Total insert time: " << duration << " ms" << std::endl;
        std::cout << "Average insert time: " << average_time << " ns" << std::endl;
    }
}

TEST_CASE("Concurrent Insertion Test")
{
    constexpr int NUM_THREADS = 4;
    constexpr int INSERTIONS_PER_THREAD = 1000;

    LookTable<int> lookTable;

    std::vector<std::thread> threads;

    // 启动多个线程并发插入数据
    for (int i = 0; i < NUM_THREADS; ++i)
    {
        threads.emplace_back([&, i]()
                             {
            for (int j = 0; j < INSERTIONS_PER_THREAD; ++j)
            {
                int id = i * INSERTIONS_PER_THREAD + j;
                lookTable.insert(id, std::move(id));
            } });
    }

    for (auto &thread : threads)
    {
        thread.join();
    }
    // std::cout << threads.size() << std::endl;
    // std::cout << lookTable.size() << std::endl;

    // 等待所有线程完成

    // // 验证插入的数据数量是否正确
    int expectedNumInsertions = NUM_THREADS * INSERTIONS_PER_THREAD;
    REQUIRE(lookTable.size() == expectedNumInsertions);
}

TEST_CASE("Concurrent Insert and Erase Test")
{
    constexpr int NUM_THREADS = 4;
    constexpr int OPERATIONS_PER_THREAD = 100;

    LookTable<int> lookTable;

    std::vector<std::thread> threads;

    // 启动多个线程并发插入和删除数据
    for (int i = 0; i < NUM_THREADS; ++i)
    {
        threads.emplace_back([&, i]()
                             {
            for (int j = 0; j < OPERATIONS_PER_THREAD; ++j)
            {
                int id = i * OPERATIONS_PER_THREAD + j;
                lookTable.insert(id, std::move(id));
                lookTable.erase(id);
            } });
    }

    // 等待所有线程完成
    for (auto &thread : threads)
    {
        thread.join();
    }

    // 验证所有数据项都已删除
    for (int i = 0; i < NUM_THREADS * OPERATIONS_PER_THREAD; ++i)
    {
        REQUIRE_FALSE(lookTable.find(i).has_value());
    }
}