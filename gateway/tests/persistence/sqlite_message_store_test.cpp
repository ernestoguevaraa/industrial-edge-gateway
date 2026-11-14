#include "ieg/persistence/message_store.hpp"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ieg::persistence {
namespace {

using namespace std::chrono_literals;
using namespace std::string_literals;

std::vector<MessageId> ids_of(const std::vector<StoredMessage>& messages)
{
    std::vector<MessageId> ids;
    ids.reserve(messages.size());
    for (const auto& message : messages) {
        ids.push_back(message.id);
    }
    return ids;
}

std::vector<std::string> payloads_of(const std::vector<StoredMessage>& messages)
{
    std::vector<std::string> payloads;
    payloads.reserve(messages.size());
    for (const auto& message : messages) {
        payloads.push_back(message.payload);
    }
    return payloads;
}

StoreTimestamp at_ms(std::int64_t milliseconds)
{
    return StoreTimestamp{std::chrono::milliseconds{milliseconds}};
}

// Direct SQLite access to the store file, for checks that must bypass the store (schema, files
// written by other tools). Only used while no store instance holds the file.
class RawDatabase
{
public:
    explicit RawDatabase(const std::filesystem::path& path)
    {
        sqlite3* raw = nullptr;
        const int code = sqlite3_open_v2(path.c_str(), &raw,
                                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
        db_.reset(raw);
        if (code != SQLITE_OK) {
            throw std::runtime_error("cannot open test database");
        }
    }

    void exec(const std::string& sql)
    {
        char* error = nullptr;
        if (sqlite3_exec(db_.get(), sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error != nullptr ? error : "unknown error";
            sqlite3_free(error);
            throw std::runtime_error(sql + ": " + message);
        }
    }

    // Every row of a query, columns rendered as text ("NULL" for SQL NULL).
    std::vector<std::vector<std::string>> rows(const std::string& sql)
    {
        sqlite3_stmt* raw = nullptr;
        if (sqlite3_prepare_v2(db_.get(), sql.c_str(), -1, &raw, nullptr) != SQLITE_OK) {
            throw std::runtime_error("cannot prepare " + sql);
        }
        const std::unique_ptr<sqlite3_stmt, int (*)(sqlite3_stmt*)> statement{raw,
                                                                             sqlite3_finalize};
        std::vector<std::vector<std::string>> result;
        while (sqlite3_step(statement.get()) == SQLITE_ROW) {
            std::vector<std::string> row;
            for (int column = 0; column < sqlite3_column_count(statement.get()); ++column) {
                row.push_back(column_text(statement.get(), column));
            }
            result.push_back(std::move(row));
        }
        return result;
    }

    std::string scalar(const std::string& sql)
    {
        const auto result = rows(sql);
        if (result.empty() || result.front().empty()) {
            throw std::runtime_error("no result for " + sql);
        }
        return result.front().front();
    }

private:
    static std::string column_text(sqlite3_stmt* statement, int column)
    {
        if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
            return "NULL";
        }
        const void* data = sqlite3_column_blob(statement, column);
        const auto size = static_cast<std::size_t>(sqlite3_column_bytes(statement, column));
        return data == nullptr ? std::string{} : std::string{static_cast<const char*>(data), size};
    }

    struct Closer
    {
        void operator()(sqlite3* db) const noexcept { sqlite3_close_v2(db); }
    };
    std::unique_ptr<sqlite3, Closer> db_;
};

class SqliteMessageStoreTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        directory_ = std::filesystem::temp_directory_path() /
                     ("ieg-store-" + std::to_string(::getpid()) + "-" +
                      ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(directory_);
        std::filesystem::create_directories(directory_);
        path_ = directory_ / "store.db";
    }

    [[nodiscard]] const std::filesystem::path& directory() const { return directory_; }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

    void TearDown() override
    {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    [[nodiscard]] MessageStoreOptions options(std::size_t max_messages = 1000,
                                              std::uint64_t max_bytes = 1'000'000,
                                              OverflowPolicy overflow =
                                                  OverflowPolicy::DropOldest) const
    {
        return MessageStoreOptions{.path = path(),
                                   .max_messages = max_messages,
                                   .max_bytes = max_bytes,
                                   .overflow = overflow,
                                   .sync = SyncMode::Full};
    }

    [[nodiscard]] std::unique_ptr<SqliteMessageStore> open() const
    {
        return std::make_unique<SqliteMessageStore>(options());
    }

    static void expect_store_error(const std::function<void()>& action, StoreErrorKind kind)
    {
        try {
            action();
            ADD_FAILURE() << "expected StoreError " << to_string(kind);
        } catch (const StoreError& error) {
            EXPECT_EQ(error.kind(), kind) << error.what();
        }
    }

private:
    std::filesystem::path directory_;
    std::filesystem::path path_;
};

// ---- Opening and schema --------------------------------------------------------------------

TEST_F(SqliteMessageStoreTest, CreatesFileWithTheProposedSchema)
{
    {
        const auto store = open();
        EXPECT_TRUE(store->empty());
        EXPECT_EQ(store->size(), 0U);
    }
    ASSERT_TRUE(std::filesystem::exists(path()));

    RawDatabase db(path());
    // cid | name | type | notnull | dflt_value | pk
    const std::vector<std::vector<std::string>> expected{
        {"0", "id", "INTEGER", "0", "NULL", "1"},
        {"1", "topic", "TEXT", "1", "NULL", "0"},
        {"2", "payload", "BLOB", "1", "NULL", "0"},
        {"3", "created_at_ms", "INTEGER", "1", "NULL", "0"},
        {"4", "attempts", "INTEGER", "1", "0", "0"}};
    EXPECT_EQ(db.rows("PRAGMA table_info(pending_messages)"), expected);
    EXPECT_NE(db.scalar("SELECT sql FROM sqlite_master WHERE name = 'pending_messages'")
                  .find("AUTOINCREMENT"),
              std::string::npos);
    EXPECT_EQ(db.scalar("PRAGMA user_version"), "1");
    EXPECT_EQ(db.scalar("PRAGMA journal_mode"), "wal");
}

TEST_F(SqliteMessageStoreTest, RejectsInvalidOptions)
{
    auto no_path = options();
    no_path.path.clear();
    EXPECT_THROW(SqliteMessageStore{no_path}, std::invalid_argument);
    EXPECT_THROW(SqliteMessageStore{options(0)}, std::invalid_argument);
    EXPECT_THROW(SqliteMessageStore{options(10, 0)}, std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(path()));
}

TEST_F(SqliteMessageStoreTest, MissingDirectoryIsCannotOpen)
{
    auto missing = options();
    missing.path = directory() / "missing" / "store.db";
    expect_store_error([&] { const SqliteMessageStore store{missing}; }, StoreErrorKind::CannotOpen);
}

TEST_F(SqliteMessageStoreTest, NonDatabaseFileIsCorrupt)
{
    std::ofstream{path(), std::ios::binary} << std::string(4096, 'x');
    expect_store_error([&] { const SqliteMessageStore store{options()}; }, StoreErrorKind::Corrupt);
}

TEST_F(SqliteMessageStoreTest, DamagedPagesAreCorrupt)
{
    {
        const auto store = open();
        for (int i = 0; i < 200; ++i) {
            store->enqueue("plant/t", std::string(100, 'v'), at_ms(i));
        }
    } // clean close: the write-ahead log is checkpointed into the file and removed
    ASSERT_GT(std::filesystem::file_size(path()), 4U * 4096U);
    {
        // Keep the header page intact, overwrite the table's b-tree pages with garbage.
        std::fstream file{path(), std::ios::binary | std::ios::in | std::ios::out};
        file.seekp(4096);
        const std::string garbage(std::size_t{3} * 4096, '\xA5');
        file.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
    }
    expect_store_error([&] { const SqliteMessageStore store{options()}; }, StoreErrorKind::Corrupt);
}

TEST_F(SqliteMessageStoreTest, DamageInUnusedPagesIsDetectedAtOpen)
{
    // Pages of acknowledged messages go to the freelist. Reading the backlog never touches them,
    // but a later enqueue reuses them, so the integrity check must catch their damage at open.
    {
        const auto store = open();
        for (int i = 0; i < 200; ++i) {
            store->enqueue("plant/t", std::string(100, 'v'), at_ms(i));
        }
        store->acknowledge(ids_of(store->peek(200)));
    }
    std::fstream file{path(), std::ios::binary | std::ios::in | std::ios::out};
    const auto read_u32 = [&file](std::streamoff offset) { // big-endian, as in the file format
        std::array<char, 4> bytes{};
        file.seekg(offset);
        file.read(bytes.data(), 4);
        std::uint32_t value = 0;
        for (const char byte : bytes) {
            value = (value << 8U) | static_cast<unsigned char>(byte);
        }
        return value;
    };
    constexpr std::streamoff kPageSize = 4096; // SQLite's default
    const std::uint32_t trunk = read_u32(32);  // header: first freelist trunk page
    ASSERT_GT(trunk, 1U);
    // Trunk page bytes 4..7: number of leaf pages it lists. Claim far more than fit on a page.
    file.seekp((static_cast<std::streamoff>(trunk) - 1) * kPageSize + 4);
    const std::array<char, 4> huge{'\x7f', '\x00', '\x00', '\x00'};
    file.write(huge.data(), 4);
    file.close();

    expect_store_error([&] { const SqliteMessageStore store{options()}; },
                       StoreErrorKind::Corrupt);
}

TEST_F(SqliteMessageStoreTest, ForeignDatabaseIsIncompatibleAndLeftUntouched)
{
    {
        RawDatabase db(path());
        db.exec("CREATE TABLE other (x INTEGER)");
    }
    expect_store_error([&] { const SqliteMessageStore store{options()}; }, StoreErrorKind::Incompatible);
    RawDatabase db(path());
    EXPECT_EQ(db.scalar("SELECT count(*) FROM sqlite_master WHERE name = 'pending_messages'"),
              "0");
    EXPECT_EQ(db.scalar("PRAGMA journal_mode"), "delete"); // not switched to WAL either
}

TEST_F(SqliteMessageStoreTest, NewerSchemaVersionIsIncompatible)
{
    {
        const auto store = open();
    }
    {
        RawDatabase db(path());
        db.exec("PRAGMA user_version = 2");
    }
    expect_store_error([&] { const SqliteMessageStore store{options()}; }, StoreErrorKind::Incompatible);
}

TEST_F(SqliteMessageStoreTest, SecondInstanceOnTheSameFileIsBusyUntilTheFirstCloses)
{
    auto first = open();
    ASSERT_TRUE(first->enqueue("t", "a", at_ms(1)).stored());
    expect_store_error([&] { const SqliteMessageStore second{options()}; }, StoreErrorKind::Busy);
    // The failed attempt must not have disturbed the first instance.
    EXPECT_TRUE(first->enqueue("t", "b", at_ms(2)).stored());

    first.reset();
    const auto reopened = open();
    EXPECT_EQ(payloads_of(reopened->peek(10)), (std::vector<std::string>{"a", "b"}));
}

// ---- Insert and query ----------------------------------------------------------------------

TEST_F(SqliteMessageStoreTest, PeekReturnsMessagesInFifoOrderWithoutRemovingThem)
{
    const auto store = open();
    const auto first = store->enqueue("plant/a", "1", at_ms(1000));
    const auto second = store->enqueue("plant/b", "2", at_ms(500)); // older timestamp, later id
    const auto third = store->enqueue("plant/c", "3", at_ms(2000));
    ASSERT_EQ(first.status, EnqueueStatus::Stored);
    EXPECT_LT(first.id, second.id);
    EXPECT_LT(second.id, third.id);

    const auto messages = store->peek(10);
    ASSERT_EQ(messages.size(), 3U);
    EXPECT_EQ(messages[0], (StoredMessage{.id = first.id,
                                          .topic = "plant/a",
                                          .payload = "1",
                                          .created_at = at_ms(1000),
                                          .attempts = 0}));
    EXPECT_EQ(ids_of(messages), (std::vector<MessageId>{first.id, second.id, third.id}));
    EXPECT_EQ(messages[1].created_at, at_ms(500));

    EXPECT_EQ(store->size(), 3U); // peek does not consume
    EXPECT_EQ(store->peek(10), messages);
}

TEST_F(SqliteMessageStoreTest, PeekHonoursLimitAndAfter)
{
    const auto store = open();
    std::vector<MessageId> ids;
    for (int i = 0; i < 5; ++i) {
        ids.push_back(store->enqueue("t", std::to_string(i), at_ms(i)).id);
    }
    EXPECT_TRUE(store->peek(0).empty());
    EXPECT_EQ(payloads_of(store->peek(2)), (std::vector<std::string>{"0", "1"}));
    EXPECT_EQ(payloads_of(store->peek(2, ids[1])), (std::vector<std::string>{"2", "3"}));
    EXPECT_EQ(payloads_of(store->peek(10, ids[3])), (std::vector<std::string>{"4"}));
    EXPECT_TRUE(store->peek(10, ids[4]).empty());
}

TEST_F(SqliteMessageStoreTest, PayloadIsStoredAsExactBytes)
{
    const auto store = open();
    const std::string binary = "\x00\x01\xff{\"v\":1}\x00"s;
    const std::string utf8_topic = "plant/\xc3\xa4rea/temp"; // "ärea": 2 bytes for one character
    store->enqueue(utf8_topic, binary, at_ms(1));
    store->enqueue("plant/empty", "", at_ms(2));
    store->enqueue("plant/null", std::string_view{}, at_ms(3)); // data() == nullptr

    const auto messages = store->peek(10);
    ASSERT_EQ(messages.size(), 3U);
    EXPECT_EQ(messages[0].topic, utf8_topic);
    EXPECT_EQ(messages[0].payload, binary);
    EXPECT_EQ(messages[1].payload, "");
    EXPECT_EQ(messages[2].payload, "");
    // Byte accounting counts bytes, not characters.
    EXPECT_EQ(store->stats().bytes,
              utf8_topic.size() + binary.size() + "plant/empty"s.size() + "plant/null"s.size());
}

TEST_F(SqliteMessageStoreTest, PayloadColumnHoldsBlobs)
{
    {
        const auto store = open();
        store->enqueue("t", "{}", at_ms(1));
        store->enqueue("t", "", at_ms(2));
        store->enqueue("t", std::string_view{}, at_ms(3));
    }
    RawDatabase db(path());
    EXPECT_EQ(db.rows("SELECT typeof(topic), typeof(payload) FROM pending_messages ORDER BY id"),
              (std::vector<std::vector<std::string>>{
                  {"text", "blob"}, {"text", "blob"}, {"text", "blob"}}));
}

TEST_F(SqliteMessageStoreTest, RejectsInvalidTopics)
{
    const auto store = open();
    EXPECT_THROW(store->enqueue("", "x", at_ms(1)), std::invalid_argument);
    EXPECT_THROW(store->enqueue("a\0b"s, "x", at_ms(1)), std::invalid_argument);
    EXPECT_TRUE(store->empty());
}

TEST_F(SqliteMessageStoreTest, EnqueueWithoutTimestampUsesTheCurrentTime)
{
    const auto store = open();
    const auto before = std::chrono::time_point_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now());
    store->enqueue("t", "x");
    const auto after = std::chrono::time_point_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now());
    const auto messages = store->peek(1);
    ASSERT_EQ(messages.size(), 1U);
    EXPECT_GE(messages[0].created_at, before);
    EXPECT_LE(messages[0].created_at, after);
}

// ---- Confirmation (acknowledge), attempts, delete ------------------------------------------

TEST_F(SqliteMessageStoreTest, AcknowledgeDeletesOnlyTheConfirmedMessages)
{
    const auto store = open();
    std::vector<MessageId> ids;
    for (int i = 0; i < 4; ++i) {
        ids.push_back(store->enqueue("t", std::to_string(i), at_ms(i)).id);
    }
    EXPECT_EQ(store->acknowledge(ids[1]), 1U);
    EXPECT_EQ(payloads_of(store->peek(10)), (std::vector<std::string>{"0", "2", "3"}));

    const std::vector<MessageId> batch{ids[0], ids[3]};
    EXPECT_EQ(store->acknowledge(batch), 2U);
    EXPECT_EQ(payloads_of(store->peek(10)), (std::vector<std::string>{"2"}));
    EXPECT_EQ(store->stats().bytes, 2U); // "t" + "2"
    EXPECT_EQ(store->stats().acknowledged, 3U);
}

TEST_F(SqliteMessageStoreTest, AcknowledgeIsIdempotentAndIgnoresUnknownIds)
{
    const auto store = open();
    const auto id = store->enqueue("t", "x", at_ms(1)).id;
    EXPECT_EQ(store->acknowledge(id), 1U);
    EXPECT_EQ(store->acknowledge(id), 0U);
    EXPECT_EQ(store->acknowledge(id + 1000), 0U);
    EXPECT_EQ(store->acknowledge(std::span<const MessageId>{}), 0U);
    EXPECT_TRUE(store->empty());
    EXPECT_EQ(store->stats().bytes, 0U);
    EXPECT_EQ(store->stats().acknowledged, 1U);
}

TEST_F(SqliteMessageStoreTest, RecordAttemptIncrementsAttempts)
{
    const auto store = open();
    const auto a = store->enqueue("t", "a", at_ms(1)).id;
    const auto b = store->enqueue("t", "b", at_ms(2)).id;
    EXPECT_EQ(store->record_attempt(a), 1U);
    const std::vector<MessageId> both{a, b, b + 1000};
    EXPECT_EQ(store->record_attempt(both), 2U);

    const auto messages = store->peek(10);
    ASSERT_EQ(messages.size(), 2U);
    EXPECT_EQ(messages[0].attempts, 2);
    EXPECT_EQ(messages[1].attempts, 1);
    EXPECT_EQ(store->size(), 2U); // attempts never remove a message
}

// ---- Limits --------------------------------------------------------------------------------

TEST_F(SqliteMessageStoreTest, MessageLimitDropsTheOldest)
{
    const auto store = std::make_unique<SqliteMessageStore>(options(3));
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(store->enqueue("t", std::to_string(i), at_ms(i)).status, EnqueueStatus::Stored);
    }
    const auto result = store->enqueue("t", "3", at_ms(3));
    EXPECT_EQ(result.status, EnqueueStatus::StoredEvictedOldest);
    EXPECT_EQ(result.evicted, 1U);
    EXPECT_TRUE(result.stored());
    EXPECT_EQ(payloads_of(store->peek(10)), (std::vector<std::string>{"1", "2", "3"}));

    const auto stats = store->stats();
    EXPECT_EQ(stats.messages, 3U);
    EXPECT_EQ(stats.enqueued, 4U);
    EXPECT_EQ(stats.evicted, 1U);
    EXPECT_EQ(stats.rejected, 0U);
}

TEST_F(SqliteMessageStoreTest, ByteLimitDropsAsManyOldestAsNeeded)
{
    // Each small message is 1 (topic) + 3 (payload) = 4 bytes; the limit is 12 bytes.
    const auto store = std::make_unique<SqliteMessageStore>(options(100, 12));
    store->enqueue("t", "aaa", at_ms(1));
    store->enqueue("t", "bbb", at_ms(2));
    store->enqueue("t", "ccc", at_ms(3));
    EXPECT_EQ(store->stats().bytes, 12U);

    // 1 + 7 = 8 bytes: two of the three old messages must go.
    const auto result = store->enqueue("t", "ddddddd", at_ms(4));
    EXPECT_EQ(result.status, EnqueueStatus::StoredEvictedOldest);
    EXPECT_EQ(result.evicted, 2U);
    EXPECT_EQ(payloads_of(store->peek(10)), (std::vector<std::string>{"ccc", "ddddddd"}));
    EXPECT_EQ(store->stats().bytes, 12U);
}

TEST_F(SqliteMessageStoreTest, ByteAccountingCountsTopicBytesNotCharacters)
{
    const std::string topic = "\xc3\xa4"; // "ä": one character, two bytes
    {
        // 2 + 1 = 3 bytes per message; room for exactly three.
        const auto store = std::make_unique<SqliteMessageStore>(options(100, 9));
        store->enqueue(topic, "a", at_ms(1));
        store->enqueue(topic, "b", at_ms(2));
        store->enqueue(topic, "c", at_ms(3));
        const auto result = store->enqueue(topic, "d", at_ms(4));
        EXPECT_EQ(result.evicted, 1U);
        EXPECT_EQ(payloads_of(store->peek(10)), (std::vector<std::string>{"b", "c", "d"}));
    }
    const auto store = std::make_unique<SqliteMessageStore>(options(100, 9));
    EXPECT_EQ(store->stats().bytes, 9U); // recomputed from the file
    store->acknowledge(store->peek(1)[0].id);
    EXPECT_EQ(store->stats().bytes, 6U);
    EXPECT_EQ(store->enqueue(topic, "e", at_ms(5)).status, EnqueueStatus::Stored);
}

TEST_F(SqliteMessageStoreTest, MessageLargerThanTheByteLimitIsRejected)
{
    const auto store = std::make_unique<SqliteMessageStore>(options(100, 10));
    store->enqueue("t", "keep", at_ms(1));
    const auto result = store->enqueue("t", "0123456789", at_ms(2)); // 11 bytes
    EXPECT_EQ(result, (EnqueueResult{.status = EnqueueStatus::RejectedTooLarge, .id = 0,
                                     .evicted = 0}));
    EXPECT_FALSE(result.stored());
    EXPECT_EQ(payloads_of(store->peek(10)), (std::vector<std::string>{"keep"}));
    EXPECT_EQ(store->stats().rejected, 1U);
}

TEST_F(SqliteMessageStoreTest, RejectNewKeepsTheBacklog)
{
    const auto store =
        std::make_unique<SqliteMessageStore>(options(2, 1'000, OverflowPolicy::RejectNew));
    const auto first = store->enqueue("t", "a", at_ms(1));
    store->enqueue("t", "b", at_ms(2));
    const auto result = store->enqueue("t", "c", at_ms(3));
    EXPECT_EQ(result.status, EnqueueStatus::RejectedFull);
    EXPECT_EQ(payloads_of(store->peek(10)), (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(store->stats().rejected, 1U);

    // Room again once a message is confirmed.
    store->acknowledge(first.id);
    EXPECT_EQ(store->enqueue("t", "c", at_ms(3)).status, EnqueueStatus::Stored);
    EXPECT_EQ(payloads_of(store->peek(10)), (std::vector<std::string>{"b", "c"}));
}

TEST_F(SqliteMessageStoreTest, RejectNewAlsoAppliesTheByteLimit)
{
    const auto store =
        std::make_unique<SqliteMessageStore>(options(100, 6, OverflowPolicy::RejectNew));
    store->enqueue("t", "ab", at_ms(1)); // 3 bytes
    EXPECT_EQ(store->enqueue("t", "cd", at_ms(2)).status, EnqueueStatus::Stored); // 6 bytes
    EXPECT_EQ(store->enqueue("t", "", at_ms(3)).status, EnqueueStatus::RejectedFull);
}

// ---- Recovery after restart / crash --------------------------------------------------------

TEST_F(SqliteMessageStoreTest, BacklogSurvivesReopenInOrderWithAttempts)
{
    MessageId first = 0;
    {
        const auto store = open();
        first = store->enqueue("plant/a", "1", at_ms(10)).id;
        store->enqueue("plant/b", "2", at_ms(20));
        store->enqueue("plant/c", "3", at_ms(30));
        store->record_attempt(first);
        store->acknowledge(store->peek(2)[1].id); // "2" was delivered
    }
    const auto store = open();
    const auto messages = store->peek(10);
    ASSERT_EQ(messages.size(), 2U);
    EXPECT_EQ(messages[0], (StoredMessage{.id = first,
                                          .topic = "plant/a",
                                          .payload = "1",
                                          .created_at = at_ms(10),
                                          .attempts = 1}));
    EXPECT_EQ(messages[1].payload, "3");

    // Totals are recovered from the file; event counters restart with the instance.
    const auto stats = store->stats();
    EXPECT_EQ(stats.messages, 2U);
    EXPECT_EQ(stats.bytes, 2U * (std::string_view{"plant/a"}.size() + 1));
    EXPECT_EQ(stats.enqueued, 0U);

    // New messages continue after the recovered ones.
    const auto next = store->enqueue("plant/d", "4", at_ms(40));
    EXPECT_GT(next.id, messages[1].id);
    EXPECT_EQ(payloads_of(store->peek(10)), (std::vector<std::string>{"1", "3", "4"}));
}

TEST_F(SqliteMessageStoreTest, IdsAreNeverReusedAfterTheStoreWasEmptied)
{
    MessageId last = 0;
    {
        const auto store = open();
        store->enqueue("t", "a", at_ms(1));
        last = store->enqueue("t", "b", at_ms(2)).id;
        store->acknowledge(ids_of(store->peek(10)));
        EXPECT_TRUE(store->empty());
        EXPECT_GT(store->enqueue("t", "c", at_ms(3)).id, last);
        last = store->peek(1)[0].id;
        store->acknowledge(last);
    }
    // A stale acknowledge from before the restart can never hit a new message.
    const auto store = open();
    const auto next = store->enqueue("t", "d", at_ms(4));
    EXPECT_GT(next.id, last);
}

TEST_F(SqliteMessageStoreTest, CommittedMessagesSurviveAProcessCrash)
{
    // The child process enqueues, then exits without destructors or sqlite3_close: the store's
    // write-ahead log is left behind exactly as after a crash.
    EXPECT_EXIT(
        {
            SqliteMessageStore crashing{options()};
            for (int i = 0; i < 50; ++i) {
                crashing.enqueue("plant/t", std::to_string(i), at_ms(i));
            }
            crashing.acknowledge(crashing.peek(1)[0].id);
            std::_Exit(0);
        },
        ::testing::ExitedWithCode(0), "");
    // The premise: nothing was checkpointed, the committed data is only in the write-ahead log.
    ASSERT_TRUE(std::filesystem::exists(path().string() + "-wal"));

    const auto store = open();
    const auto messages = store->peek(100);
    ASSERT_EQ(messages.size(), 49U);
    EXPECT_EQ(messages.front().payload, "1");
    EXPECT_EQ(messages.back().payload, "49");
    EXPECT_EQ(store->stats().bytes, 7U * 49U + 9U + 40U * 2U);
    EXPECT_TRUE(store->enqueue("plant/t", "50", at_ms(50)).stored());
}

TEST_F(SqliteMessageStoreTest, ReopenWithSmallerLimitsTrimsTheOldest)
{
    {
        const auto store = open();
        for (int i = 0; i < 5; ++i) {
            store->enqueue("t", std::to_string(i), at_ms(i));
        }
    }
    {
        const auto store = std::make_unique<SqliteMessageStore>(options(3));
        EXPECT_EQ(payloads_of(store->peek(10)), (std::vector<std::string>{"2", "3", "4"}));
        EXPECT_EQ(store->stats().evicted, 2U);
    }
    {
        const auto store = std::make_unique<SqliteMessageStore>(options(100, 4)); // 2 bytes each
        EXPECT_EQ(payloads_of(store->peek(10)), (std::vector<std::string>{"3", "4"}));
    }
}

TEST_F(SqliteMessageStoreTest, ReopenWithRejectNewKeepsAnOversizedBacklog)
{
    {
        const auto store = open();
        for (int i = 0; i < 3; ++i) {
            store->enqueue("t", std::to_string(i), at_ms(i));
        }
    }
    const auto store =
        std::make_unique<SqliteMessageStore>(options(1, 1'000, OverflowPolicy::RejectNew));
    EXPECT_EQ(store->size(), 3U);
    EXPECT_EQ(store->enqueue("t", "x", at_ms(9)).status, EnqueueStatus::RejectedFull);
}

// ---- Concurrency ---------------------------------------------------------------------------

TEST_F(SqliteMessageStoreTest, ConcurrentProducersAndConsumerKeepConsistentTotals)
{
    auto store_options = options(100'000);
    store_options.sync = SyncMode::Normal; // keeps the test fast; the locking is the same
    SqliteMessageStore store{store_options};
    constexpr std::size_t kThreads = 4;
    constexpr std::size_t kPerThread = 100;
    constexpr std::size_t kTotal = kThreads * kPerThread;

    std::vector<std::future<std::vector<MessageId>>> producers;
    producers.reserve(kThreads);
    for (std::size_t t = 0; t < kThreads; ++t) {
        producers.push_back(std::async(std::launch::async, [&store, t] {
            std::vector<MessageId> ids;
            for (std::size_t i = 0; i < kPerThread; ++i) {
                ids.push_back(store.enqueue("t/" + std::to_string(t), std::to_string(i)).id);
            }
            return ids;
        }));
    }
    std::size_t acknowledged = 0;
    auto consumer = std::async(std::launch::async, [&store, &acknowledged] {
        const auto deadline = std::chrono::steady_clock::now() + 20s;
        while (acknowledged < 100 && std::chrono::steady_clock::now() < deadline) {
            acknowledged += store.acknowledge(ids_of(store.peek(10)));
        }
    });

    std::set<MessageId> unique;
    for (auto& producer : producers) {
        const auto ids = producer.get();
        // Each producer sees its own messages in increasing id order.
        EXPECT_TRUE(std::is_sorted(ids.begin(), ids.end()));
        unique.insert(ids.begin(), ids.end());
    }
    consumer.get();
    EXPECT_EQ(unique.size(), kTotal);
    EXPECT_GE(acknowledged, 100U);

    const auto remaining = store.peek(kTotal);
    EXPECT_EQ(remaining.size(), kTotal - acknowledged);
    EXPECT_EQ(store.size(), remaining.size());
    EXPECT_TRUE(std::is_sorted(remaining.begin(), remaining.end(),
                               [](const auto& a, const auto& b) { return a.id < b.id; }));
    std::uint64_t bytes = 0;
    for (const auto& message : remaining) {
        bytes += message.topic.size() + message.payload.size();
    }
    EXPECT_EQ(store.stats().bytes, bytes);
}

TEST(EnqueueStatusTest, ToString)
{
    EXPECT_EQ(to_string(EnqueueStatus::Stored), "Stored");
    EXPECT_EQ(to_string(EnqueueStatus::StoredEvictedOldest), "StoredEvictedOldest");
    EXPECT_EQ(to_string(EnqueueStatus::RejectedFull), "RejectedFull");
    EXPECT_EQ(to_string(EnqueueStatus::RejectedTooLarge), "RejectedTooLarge");
    EXPECT_EQ(to_string(StoreErrorKind::Busy), "Busy");
    EXPECT_EQ(to_string(StoreErrorKind::Incompatible), "Incompatible");
}

} // namespace
} // namespace ieg::persistence
