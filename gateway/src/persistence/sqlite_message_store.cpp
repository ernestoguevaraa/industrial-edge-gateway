#include "ieg/persistence/message_store.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// All SQLite access happens under Impl::mutex_, on a connection opened with SQLITE_OPEN_NOMUTEX
// (the store serializes itself). The connection runs in exclusive locking mode, so after the
// first write no other connection can read or modify the file; the in-memory message and byte
// counters therefore always match the table.

namespace ieg::persistence {
namespace {

// Schema version stored in PRAGMA user_version. Bump it (and add a migration) on schema changes.
constexpr int kSchemaVersion = 1;

// RETURNING (acknowledge) needs SQLite 3.35.
constexpr int kMinSqliteVersion = 3'035'000;

constexpr std::string_view kCreateSchema = R"sql(
CREATE TABLE pending_messages (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    topic TEXT NOT NULL,
    payload BLOB NOT NULL,
    created_at_ms INTEGER NOT NULL,
    attempts INTEGER NOT NULL DEFAULT 0
);
)sql";

// The size of one message for the max_bytes limit is
// `length(CAST(topic AS BLOB)) + length(payload)`: CAST counts the topic in bytes, not characters.
constexpr const char* kInsertSql =
    "INSERT INTO pending_messages (topic, payload, created_at_ms) VALUES (?1, ?2, ?3)";
constexpr const char* kPeekSql = "SELECT id, topic, payload, created_at_ms, attempts "
                                 "FROM pending_messages WHERE id > ?1 ORDER BY id LIMIT ?2";
constexpr const char* kAcknowledgeSql = "DELETE FROM pending_messages WHERE id = ?1 "
                                        "RETURNING length(CAST(topic AS BLOB)) + length(payload)";
constexpr const char* kAttemptSql =
    "UPDATE pending_messages SET attempts = attempts + 1 WHERE id = ?1";
constexpr const char* kOldestSql = "SELECT id, length(CAST(topic AS BLOB)) + length(payload) "
                                   "FROM pending_messages ORDER BY id";
constexpr const char* kDeleteUpToSql = "DELETE FROM pending_messages WHERE id <= ?1";
constexpr const char* kTotalsSql =
    "SELECT count(*), coalesce(sum(length(CAST(topic AS BLOB)) + length(payload)), 0) "
    "FROM pending_messages";

// Equivalent to SQLITE_STATIC (a null destructor): SQLite does not copy the bound bytes, which
// must stay valid until the statement is reset and its bindings cleared (see StatementUse).
constexpr sqlite3_destructor_type kBindStatic = nullptr;

StoreErrorKind kind_of(int code) noexcept
{
    switch (code & 0xFF) { // primary result code
    case SQLITE_CANTOPEN:
    case SQLITE_PERM:
    case SQLITE_READONLY:
    case SQLITE_AUTH:
        return StoreErrorKind::CannotOpen;
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
        return StoreErrorKind::Busy;
    case SQLITE_CORRUPT:
    case SQLITE_NOTADB:
        return StoreErrorKind::Corrupt;
    case SQLITE_FULL:
        return StoreErrorKind::Full;
    case SQLITE_IOERR:
        return StoreErrorKind::Io;
    default:
        return StoreErrorKind::Internal;
    }
}

struct DatabaseCloser
{
    void operator()(sqlite3* db) const noexcept { sqlite3_close_v2(db); }
};
using DatabaseHandle = std::unique_ptr<sqlite3, DatabaseCloser>;

struct StatementFinalizer
{
    void operator()(sqlite3_stmt* statement) const noexcept { sqlite3_finalize(statement); }
};
using StatementHandle = std::unique_ptr<sqlite3_stmt, StatementFinalizer>;

std::int64_t to_sql_integer(std::uint64_t value)
{
    return static_cast<std::int64_t>(
        std::min<std::uint64_t>(value, std::numeric_limits<std::int64_t>::max()));
}

// One use of a cached prepared statement: binds, steps, and on destruction resets the statement
// and clears its bindings, so no statement stays active (holding a read transaction open) and no
// statically bound buffer is referenced after the call that bound it.
class StatementUse
{
public:
    StatementUse(sqlite3* db, sqlite3_stmt* statement) : db_(db), statement_(statement) {}
    ~StatementUse()
    {
        sqlite3_reset(statement_);
        sqlite3_clear_bindings(statement_);
    }
    StatementUse(const StatementUse&) = delete;
    StatementUse& operator=(const StatementUse&) = delete;
    StatementUse(StatementUse&&) = delete;
    StatementUse& operator=(StatementUse&&) = delete;

    void bind(int index, std::int64_t value)
    {
        check(sqlite3_bind_int64(statement_, index, value));
    }

    void bind_text(int index, std::string_view text)
    {
        check(sqlite3_bind_text64(statement_, index, text.data(), text.size(), kBindStatic,
                                  SQLITE_UTF8));
    }

    void bind_blob(int index, std::string_view bytes)
    {
        // A null data pointer would bind SQL NULL (violating NOT NULL): bind an empty blob.
        if (bytes.empty()) {
            check(sqlite3_bind_zeroblob(statement_, index, 0));
            return;
        }
        check(sqlite3_bind_blob64(statement_, index, bytes.data(), bytes.size(), kBindStatic));
    }

    // True for a result row, false when done.
    bool step()
    {
        const int code = sqlite3_step(statement_);
        if (code == SQLITE_ROW) {
            return true;
        }
        if (code == SQLITE_DONE) {
            return false;
        }
        fail(code);
    }

    [[nodiscard]] std::int64_t column_int(int column) const
    {
        return sqlite3_column_int64(statement_, column);
    }

    // Raw bytes of a TEXT or BLOB column.
    [[nodiscard]] std::string column_bytes(int column) const
    {
        const void* data = sqlite3_column_blob(statement_, column);
        const int size = sqlite3_column_bytes(statement_, column);
        if (data == nullptr || size <= 0) {
            return {};
        }
        return {static_cast<const char*>(data), static_cast<std::size_t>(size)};
    }

private:
    void check(int code) const
    {
        if (code != SQLITE_OK) {
            fail(code);
        }
    }

    [[noreturn]] void fail(int code) const
    {
        throw StoreError(kind_of(code), code,
                         std::string{"message store: "} + sqlite3_errmsg(db_) + " (" +
                             sqlite3_sql(statement_) + ")");
    }

    sqlite3* db_;
    sqlite3_stmt* statement_;
};

} // namespace

std::string_view to_string(EnqueueStatus status) noexcept
{
    switch (status) {
    case EnqueueStatus::Stored:
        return "Stored";
    case EnqueueStatus::StoredEvictedOldest:
        return "StoredEvictedOldest";
    case EnqueueStatus::RejectedFull:
        return "RejectedFull";
    case EnqueueStatus::RejectedTooLarge:
        return "RejectedTooLarge";
    }
    return "Unknown";
}

std::string_view to_string(StoreErrorKind kind) noexcept
{
    switch (kind) {
    case StoreErrorKind::CannotOpen:
        return "CannotOpen";
    case StoreErrorKind::Busy:
        return "Busy";
    case StoreErrorKind::Corrupt:
        return "Corrupt";
    case StoreErrorKind::Incompatible:
        return "Incompatible";
    case StoreErrorKind::Full:
        return "Full";
    case StoreErrorKind::Io:
        return "Io";
    case StoreErrorKind::Internal:
        return "Internal";
    }
    return "Unknown";
}

class SqliteMessageStore::Impl
{
public:
    explicit Impl(MessageStoreOptions options) : options_(validated(std::move(options)))
    {
        if (sqlite3_libversion_number() < kMinSqliteVersion) {
            throw StoreError(StoreErrorKind::Internal, 0,
                             std::string{"message store: SQLite >= 3.35 required, found "} +
                                 sqlite3_libversion());
        }
        open();
        configure();
        initialize();
    }

    ~Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    EnqueueResult enqueue(std::string_view topic, std::string_view payload,
                          StoreTimestamp created_at)
    {
        if (topic.empty()) {
            throw std::invalid_argument("message store: topic must not be empty");
        }
        if (topic.find('\0') != std::string_view::npos) {
            throw std::invalid_argument("message store: topic must not contain NUL");
        }
        const std::uint64_t size = topic.size() + payload.size();

        const std::lock_guard lock(mutex_);
        if (size > options_.max_bytes) {
            ++rejected_;
            return EnqueueResult{.status = EnqueueStatus::RejectedTooLarge, .id = 0, .evicted = 0};
        }
        const std::size_t excess_messages =
            count_ >= options_.max_messages ? count_ + 1 - options_.max_messages : 0;
        const std::uint64_t excess_bytes =
            bytes_ + size > options_.max_bytes ? bytes_ + size - options_.max_bytes : 0;
        const bool full = excess_messages > 0 || excess_bytes > 0;
        if (full && options_.overflow == OverflowPolicy::RejectNew) {
            ++rejected_;
            return EnqueueResult{.status = EnqueueStatus::RejectedFull, .id = 0, .evicted = 0};
        }

        Transaction transaction(*this);
        Eviction eviction;
        if (full) {
            eviction = evict_oldest(excess_messages, excess_bytes);
        }
        MessageId id = 0;
        {
            StatementUse insert(db_.get(), insert_.get());
            insert.bind_text(1, topic);
            insert.bind_blob(2, payload);
            insert.bind(3, created_at.time_since_epoch().count());
            insert.step();
            id = sqlite3_last_insert_rowid(db_.get());
        }
        transaction.commit();

        // Counters change only after a successful commit.
        count_ = count_ - eviction.messages + 1;
        bytes_ = bytes_ - eviction.bytes + size;
        ++enqueued_;
        evicted_ += eviction.messages;
        return EnqueueResult{.status = eviction.messages > 0 ? EnqueueStatus::StoredEvictedOldest
                                                             : EnqueueStatus::Stored,
                             .id = id,
                             .evicted = eviction.messages};
    }

    std::vector<StoredMessage> peek(std::size_t max_messages, MessageId after)
    {
        std::vector<StoredMessage> messages;
        if (max_messages == 0) {
            return messages;
        }
        const std::lock_guard lock(mutex_);
        messages.reserve(std::min(max_messages, count_));
        StatementUse select(db_.get(), peek_.get());
        select.bind(1, after);
        select.bind(2, to_sql_integer(max_messages));
        while (select.step()) {
            messages.push_back(StoredMessage{
                .id = select.column_int(0),
                .topic = select.column_bytes(1),
                .payload = select.column_bytes(2),
                .created_at = StoreTimestamp{std::chrono::milliseconds{select.column_int(3)}},
                .attempts = select.column_int(4)});
        }
        return messages;
    }

    std::size_t acknowledge(std::span<const MessageId> ids)
    {
        const std::lock_guard lock(mutex_);
        if (ids.empty()) {
            return 0;
        }
        Transaction transaction(*this);
        std::size_t removed = 0;
        std::uint64_t freed = 0;
        for (const MessageId id : ids) {
            StatementUse remove(db_.get(), acknowledge_.get());
            remove.bind(1, id);
            while (remove.step()) { // one RETURNING row if the id existed
                ++removed;
                freed += static_cast<std::uint64_t>(remove.column_int(0));
            }
        }
        transaction.commit();
        count_ -= removed;
        bytes_ -= freed;
        acknowledged_ += removed;
        return removed;
    }

    std::size_t record_attempt(std::span<const MessageId> ids)
    {
        const std::lock_guard lock(mutex_);
        if (ids.empty()) {
            return 0;
        }
        Transaction transaction(*this);
        std::size_t updated = 0;
        for (const MessageId id : ids) {
            StatementUse update(db_.get(), attempt_.get());
            update.bind(1, id);
            update.step();
            updated += static_cast<std::size_t>(sqlite3_changes(db_.get()));
        }
        transaction.commit();
        return updated;
    }

    [[nodiscard]] StoreStats stats()
    {
        const std::lock_guard lock(mutex_);
        return StoreStats{.messages = count_,
                          .bytes = bytes_,
                          .max_messages = options_.max_messages,
                          .max_bytes = options_.max_bytes,
                          .enqueued = enqueued_,
                          .evicted = evicted_,
                          .rejected = rejected_,
                          .acknowledged = acknowledged_};
    }

    [[nodiscard]] std::size_t size()
    {
        const std::lock_guard lock(mutex_);
        return count_;
    }

    [[nodiscard]] const MessageStoreOptions& options() const noexcept { return options_; }

private:
    struct Eviction
    {
        std::size_t messages{};
        std::uint64_t bytes{};
    };

    // BEGIN IMMEDIATE on construction; rolls back on destruction unless committed.
    class Transaction
    {
    public:
        explicit Transaction(Impl& store) : store_(store) { store_.exec("BEGIN IMMEDIATE"); }
        ~Transaction()
        {
            if (!committed_) {
                // Best effort: after some failures SQLite has already rolled back, and then this
                // reports "no transaction is active", which is fine.
                sqlite3_exec(store_.db_.get(), "ROLLBACK", nullptr, nullptr, nullptr);
            }
        }
        Transaction(const Transaction&) = delete;
        Transaction& operator=(const Transaction&) = delete;
        Transaction(Transaction&&) = delete;
        Transaction& operator=(Transaction&&) = delete;

        void commit()
        {
            store_.exec("COMMIT");
            committed_ = true;
        }

    private:
        Impl& store_;
        bool committed_{false};
    };

    static MessageStoreOptions validated(MessageStoreOptions options)
    {
        if (options.path.empty()) {
            throw std::invalid_argument("message store: path must not be empty");
        }
        if (options.max_messages == 0) {
            throw std::invalid_argument("message store: max_messages must be at least 1");
        }
        if (options.max_bytes == 0) {
            throw std::invalid_argument("message store: max_bytes must be at least 1");
        }
        return options;
    }

    [[noreturn]] void fail(int code, std::string_view what) const
    {
        throw StoreError(kind_of(code), code,
                         "message store '" + options_.path.string() + "': " + std::string{what} +
                             ": " + (db_ != nullptr ? sqlite3_errmsg(db_.get()) : sqlite3_errstr(code)));
    }

    [[noreturn]] void incompatible(const std::string& reason) const
    {
        throw StoreError(StoreErrorKind::Incompatible, 0,
                         "message store '" + options_.path.string() + "': " + reason);
    }

    void exec(const char* sql)
    {
        const int code = sqlite3_exec(db_.get(), sql, nullptr, nullptr, nullptr);
        if (code != SQLITE_OK) {
            fail(code, sql);
        }
    }

    // Single-row, single-column query (PRAGMAs and aggregates).
    std::int64_t query_int(const char* sql)
    {
        const StatementHandle statement = prepare(sql);
        StatementUse use(db_.get(), statement.get());
        if (!use.step()) {
            fail(SQLITE_INTERNAL, std::string{"no result for "} + sql);
        }
        return use.column_int(0);
    }

    StatementHandle prepare(const char* sql)
    {
        sqlite3_stmt* raw = nullptr;
        const int code = sqlite3_prepare_v3(db_.get(), sql, -1, SQLITE_PREPARE_PERSISTENT, &raw,
                                            nullptr);
        if (code != SQLITE_OK) {
            sqlite3_finalize(raw); // no-op for the null statement a failed prepare returns
            fail(code, std::string{"cannot prepare "} + sql);
        }
        return StatementHandle{raw};
    }

    void open()
    {
        sqlite3* raw = nullptr;
        const int code =
            sqlite3_open_v2(options_.path.c_str(), &raw,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                            nullptr);
        db_.reset(raw); // a handle is returned (and must be closed) even on failure
        if (code != SQLITE_OK) {
            fail(code, "cannot open");
        }
        sqlite3_extended_result_codes(db_.get(), 1);
    }

    void configure()
    {
        // Exclusive locking must precede WAL: the WAL index then lives in process memory (no
        // -shm file), and every lock taken is held until close.
        exec("PRAGMA locking_mode = EXCLUSIVE");
        // Validate before changing anything, so a file that is not ours is never modified. This
        // is the first read of the file: a non-database file fails here (Corrupt), a file held
        // by another store here or at the first write below (Busy).
        check_schema_version();
        exec("PRAGMA journal_mode = WAL");
        exec(options_.sync == SyncMode::Full ? "PRAGMA synchronous = FULL"
                                             : "PRAGMA synchronous = NORMAL");
    }

    void initialize()
    {
        {
            // The write lock makes a file that another instance has just opened fail with Busy.
            Transaction transaction(*this);
            check_integrity();
            if (check_schema_version() == 0) {
                exec(std::string{kCreateSchema}.c_str());
                exec(("PRAGMA user_version = " + std::to_string(kSchemaVersion)).c_str());
            }
            transaction.commit();
        }
        insert_ = prepare(kInsertSql);
        peek_ = prepare(kPeekSql);
        acknowledge_ = prepare(kAcknowledgeSql);
        attempt_ = prepare(kAttemptSql);
        oldest_ = prepare(kOldestSql);
        delete_up_to_ = prepare(kDeleteUpToSql);
        load_totals();
        trim_to_limits();
    }

    // kSchemaVersion, or 0 for a new, empty file. Throws Incompatible for anything else,
    // including an existing database of some other application (user_version 0 but not empty).
    std::int64_t check_schema_version()
    {
        const std::int64_t version = query_int("PRAGMA user_version");
        if (version == 0 && query_int("SELECT count(*) FROM sqlite_master") != 0) {
            incompatible("not a message store (existing SQLite database without the message "
                         "store schema version)");
        }
        if (version != 0 && version != kSchemaVersion) {
            incompatible("unsupported schema version " + std::to_string(version) +
                         " (this build supports " + std::to_string(kSchemaVersion) + ")");
        }
        return version;
    }

    void check_integrity()
    {
        const StatementHandle statement = prepare("PRAGMA quick_check(1)");
        StatementUse check(db_.get(), statement.get());
        const std::string result = check.step() ? check.column_bytes(0) : std::string{};
        if (result != "ok") {
            throw StoreError(StoreErrorKind::Corrupt, SQLITE_CORRUPT,
                             "message store '" + options_.path.string() +
                                 "': integrity check failed: " + result);
        }
    }

    void load_totals()
    {
        const StatementHandle statement = prepare(kTotalsSql);
        StatementUse totals(db_.get(), statement.get());
        if (!totals.step()) {
            fail(SQLITE_INTERNAL, "no result for totals");
        }
        count_ = static_cast<std::size_t>(std::max<std::int64_t>(totals.column_int(0), 0));
        bytes_ = static_cast<std::uint64_t>(std::max<std::int64_t>(totals.column_int(1), 0));
    }

    // A backlog left by a previous run may exceed limits that have been lowered since.
    void trim_to_limits()
    {
        const std::size_t excess_messages =
            count_ > options_.max_messages ? count_ - options_.max_messages : 0;
        const std::uint64_t excess_bytes =
            bytes_ > options_.max_bytes ? bytes_ - options_.max_bytes : 0;
        if (options_.overflow != OverflowPolicy::DropOldest ||
            (excess_messages == 0 && excess_bytes == 0)) {
            return; // RejectNew keeps the backlog; new messages are refused until it drains
        }
        Transaction transaction(*this);
        const Eviction eviction = evict_oldest(excess_messages, excess_bytes);
        transaction.commit();
        count_ -= eviction.messages;
        bytes_ -= eviction.bytes;
        evicted_ += eviction.messages;
    }

    // Deletes the oldest messages until at least `messages` messages and `bytes` bytes are freed
    // (or the table is empty). Must run inside a transaction; the caller updates the counters
    // after the commit.
    Eviction evict_oldest(std::size_t messages, std::uint64_t bytes)
    {
        Eviction eviction;
        MessageId last = 0;
        {
            StatementUse oldest(db_.get(), oldest_.get());
            while ((eviction.messages < messages || eviction.bytes < bytes) && oldest.step()) {
                last = oldest.column_int(0);
                eviction.bytes += static_cast<std::uint64_t>(oldest.column_int(1));
                ++eviction.messages;
            }
        }
        if (eviction.messages > 0) {
            StatementUse remove(db_.get(), delete_up_to_.get());
            remove.bind(1, last);
            remove.step();
        }
        return eviction;
    }

    const MessageStoreOptions options_;

    std::mutex mutex_;
    // Declared before the statements so they are finalized before the connection is closed.
    DatabaseHandle db_;
    StatementHandle insert_;
    StatementHandle peek_;
    StatementHandle acknowledge_;
    StatementHandle attempt_;
    StatementHandle oldest_;
    StatementHandle delete_up_to_;

    // Guarded by mutex_ (after construction).
    std::size_t count_{0};
    std::uint64_t bytes_{0};
    std::uint64_t enqueued_{0};
    std::uint64_t evicted_{0};
    std::uint64_t rejected_{0};
    std::uint64_t acknowledged_{0};
};

SqliteMessageStore::SqliteMessageStore(MessageStoreOptions options)
    : impl_(std::make_unique<Impl>(std::move(options)))
{}

SqliteMessageStore::~SqliteMessageStore() = default;

EnqueueResult SqliteMessageStore::enqueue(std::string_view topic, std::string_view payload,
                                          StoreTimestamp created_at)
{
    return impl_->enqueue(topic, payload, created_at);
}

EnqueueResult SqliteMessageStore::enqueue(std::string_view topic, std::string_view payload)
{
    return impl_->enqueue(
        topic, payload,
        std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now()));
}

std::vector<StoredMessage> SqliteMessageStore::peek(std::size_t max_messages,
                                                    MessageId after) const
{
    return impl_->peek(max_messages, after);
}

std::size_t SqliteMessageStore::acknowledge(std::span<const MessageId> ids)
{
    return impl_->acknowledge(ids);
}

std::size_t SqliteMessageStore::acknowledge(MessageId id)
{
    return impl_->acknowledge(std::span<const MessageId>{&id, 1});
}

std::size_t SqliteMessageStore::record_attempt(std::span<const MessageId> ids)
{
    return impl_->record_attempt(ids);
}

std::size_t SqliteMessageStore::record_attempt(MessageId id)
{
    return impl_->record_attempt(std::span<const MessageId>{&id, 1});
}

std::size_t SqliteMessageStore::size() const
{
    return impl_->size();
}

bool SqliteMessageStore::empty() const
{
    return impl_->size() == 0;
}

StoreStats SqliteMessageStore::stats() const
{
    return impl_->stats();
}

const MessageStoreOptions& SqliteMessageStore::options() const noexcept
{
    return impl_->options();
}

} // namespace ieg::persistence
