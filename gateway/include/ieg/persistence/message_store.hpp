#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Persistent, bounded FIFO of outgoing messages (topic + opaque payload) for store-and-forward:
// messages that cannot be published while the broker is unreachable are written to a local
// SQLite file and published later, in insertion order, once the connection is back. The store
// survives process restarts and reboots.
//
// This header is SDK-neutral: it never includes sqlite3.h or exposes SQLite types. The store is
// protocol-neutral as well: it knows nothing about MQTT beyond "a topic string and a payload".
//
// Delivery model (at-least-once): peek() returns the oldest messages without removing them; the
// caller publishes them and calls acknowledge() once delivery is confirmed, which deletes them.
// If the process dies between publishing and acknowledge(), the message is published again after
// the restart (a duplicate, never a loss).
namespace ieg::persistence {

// Strictly increasing in insertion order and never reused, not even after the store was emptied
// or reopened (SQLite AUTOINCREMENT), so an id identifies one message for the file's lifetime and
// ordering by id is FIFO order.
using MessageId = std::int64_t;

// Creation time as stored in `created_at_ms` (milliseconds since the Unix epoch, UTC).
using StoreTimestamp = std::chrono::sys_time<std::chrono::milliseconds>;

// What enqueue() does when the new message would exceed max_messages or max_bytes.
enum class OverflowPolicy : std::uint8_t
{
    // Delete the oldest messages until the new one fits (same policy as the in-memory
    // telemetry queue: the most recent data is kept).
    DropOldest,
    // Keep the stored messages and refuse the new one.
    RejectNew
};

// SQLite `synchronous` setting (the journal is always WAL).
enum class SyncMode : std::uint8_t
{
    // Every committed enqueue/acknowledge is flushed to stable storage (fsync) before the call
    // returns: a committed message survives a power loss. Slowest; the default.
    Full,
    // Committed changes survive a process crash but the most recent ones may be lost on a power
    // loss or OS crash; the file always stays consistent. Far fewer fsyncs (flash wear).
    Normal
};

struct MessageStoreOptions
{
    // SQLite database file; created if missing. The parent directory must exist.
    std::filesystem::path path;
    // Limits on the stored backlog, both >= 1. max_bytes counts topic + payload bytes of the
    // stored messages, not the size of the file on disk (which adds SQLite page and WAL
    // overhead and does not shrink when messages are removed).
    std::size_t max_messages{100'000};
    std::uint64_t max_bytes{std::uint64_t{64} * 1024 * 1024};
    OverflowPolicy overflow{OverflowPolicy::DropOldest};
    SyncMode sync{SyncMode::Full};
};

struct StoredMessage
{
    MessageId id{};
    std::string topic;
    // Opaque bytes, returned exactly as stored (may be empty or contain NUL bytes).
    std::string payload;
    StoreTimestamp created_at{};
    // Number of record_attempt() calls for this message; persisted across restarts.
    std::int64_t attempts{};

    friend bool operator==(const StoredMessage&, const StoredMessage&) = default;
};

enum class EnqueueStatus : std::uint8_t
{
    Stored,
    // Stored after deleting the oldest messages to stay within the limits (DropOldest).
    StoredEvictedOldest,
    // Not stored: the limits are reached and the policy is RejectNew.
    RejectedFull,
    // Not stored: topic + payload alone exceed max_bytes (under either policy).
    RejectedTooLarge
};

[[nodiscard]] std::string_view to_string(EnqueueStatus status) noexcept;

struct EnqueueResult
{
    EnqueueStatus status{EnqueueStatus::RejectedFull};
    // Id of the stored message; 0 if it was not stored.
    MessageId id{};
    // Messages deleted to make room (non-zero only for StoredEvictedOldest).
    std::size_t evicted{};

    [[nodiscard]] bool stored() const noexcept
    {
        return status == EnqueueStatus::Stored || status == EnqueueStatus::StoredEvictedOldest;
    }

    friend bool operator==(const EnqueueResult&, const EnqueueResult&) = default;
};

// Consistent snapshot. The event counters count since this store instance was opened.
struct StoreStats
{
    std::size_t messages{};     // messages currently stored
    std::uint64_t bytes{};      // their topic + payload bytes
    std::size_t max_messages{}; // configured limits
    std::uint64_t max_bytes{};
    std::uint64_t enqueued{};     // messages stored by enqueue()
    std::uint64_t evicted{};      // oldest messages deleted by the overflow policy (also at open)
    std::uint64_t rejected{};     // RejectedFull + RejectedTooLarge
    std::uint64_t acknowledged{}; // messages deleted by acknowledge()

    friend bool operator==(const StoreStats&, const StoreStats&) = default;
};

enum class StoreErrorKind : std::uint8_t
{
    // The file cannot be opened or written (missing directory, permissions, read-only).
    CannotOpen,
    // The file is locked by another store instance (this or another process).
    Busy,
    // The file is not a SQLite database or fails the integrity check.
    Corrupt,
    // A valid SQLite database, but not a message store of a schema version this build supports.
    Incompatible,
    // The disk (or the database) is full.
    Full,
    // Operating-system I/O error.
    Io,
    // Any other SQLite failure.
    Internal
};

[[nodiscard]] std::string_view to_string(StoreErrorKind kind) noexcept;

// Storage failure. Thrown by the constructor and by any operation whose SQLite call fails; the
// failed operation has no effect (its transaction is rolled back). After Corrupt or Io the
// store should be treated as unusable.
class StoreError : public std::runtime_error
{
public:
    StoreError(StoreErrorKind kind, int native_code, const std::string& message)
        : std::runtime_error(message), kind_(kind), native_code_(native_code)
    {}

    [[nodiscard]] StoreErrorKind kind() const noexcept { return kind_; }
    // SQLite extended result code, or 0 if the error was detected by the store itself.
    [[nodiscard]] int native_code() const noexcept { return native_code_; }

private:
    StoreErrorKind kind_;
    int native_code_;
};

// SQLite-backed message store. Schema (user_version 1):
//
//   CREATE TABLE pending_messages (
//       id INTEGER PRIMARY KEY AUTOINCREMENT,
//       topic TEXT NOT NULL,
//       payload BLOB NOT NULL,
//       created_at_ms INTEGER NOT NULL,
//       attempts INTEGER NOT NULL DEFAULT 0
//   );
//
// Opening (constructor): creates the file and schema if needed, otherwise recovers the existing
// backlog (including after a crash or power loss: SQLite replays or discards the write-ahead log),
// runs an integrity check, and, with DropOldest, trims a backlog that exceeds smaller limits
// configured since the last run. The instance then holds an exclusive lock on the file until it
// is destroyed; a second instance on the same file fails with StoreErrorKind::Busy.
//
// Threading: every member function is thread-safe (one internal mutex). Each call blocks on disk
// I/O (with SyncMode::Full including an fsync per modifying call), so do not call it from
// latency-sensitive adapter callbacks.
class SqliteMessageStore
{
public:
    // Throws std::invalid_argument for an empty path or a zero limit, StoreError if the file
    // cannot be opened, recovered or initialized.
    explicit SqliteMessageStore(MessageStoreOptions options);
    // Closes the database; a clean close also checkpoints and removes the write-ahead log.
    ~SqliteMessageStore();

    SqliteMessageStore(const SqliteMessageStore&) = delete;
    SqliteMessageStore& operator=(const SqliteMessageStore&) = delete;
    SqliteMessageStore(SqliteMessageStore&&) = delete;
    SqliteMessageStore& operator=(SqliteMessageStore&&) = delete;

    // Appends one message (atomically together with any eviction). Policy outcomes are reported
    // in the result, never thrown. Throws std::invalid_argument for an empty topic or a topic
    // containing NUL, StoreError if the write fails.
    EnqueueResult enqueue(std::string_view topic, std::string_view payload,
                          StoreTimestamp created_at);
    // As above, with created_at = the current system time.
    EnqueueResult enqueue(std::string_view topic, std::string_view payload);

    // Up to max_messages of the oldest stored messages with an id greater than `after`, in FIFO
    // order, without removing them. `after` lets a caller read the next batch while earlier
    // messages are still awaiting acknowledge(). Empty if there are none.
    [[nodiscard]] std::vector<StoredMessage> peek(std::size_t max_messages,
                                                  MessageId after = 0) const;

    // Deletes the given messages (delivery confirmed, or deliberately discarded) in one
    // transaction and returns how many existed. Unknown or already acknowledged ids are ignored,
    // so acknowledging twice is harmless.
    std::size_t acknowledge(std::span<const MessageId> ids);
    std::size_t acknowledge(MessageId id);

    // Increments `attempts` of the given messages (e.g. after a failed publish) in one
    // transaction and returns how many existed. Unknown ids are ignored.
    std::size_t record_attempt(std::span<const MessageId> ids);
    std::size_t record_attempt(MessageId id);

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool empty() const;
    [[nodiscard]] StoreStats stats() const;
    [[nodiscard]] const MessageStoreOptions& options() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ieg::persistence
