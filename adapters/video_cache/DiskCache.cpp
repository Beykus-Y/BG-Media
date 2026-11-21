#include "bgmedia/video/DiskCache.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <limits>
#include <map>
#include <mutex>
#include <string_view>
#include <utility>

namespace bgmedia::video {
namespace {

constexpr std::uint64_t blockSize = 64 * 1024;

class DiskError : public std::runtime_error {
public:
    DiskError() : std::runtime_error("video cache database operation failed") {}
};

void check(int result) {
    if (result != SQLITE_OK) throw DiskError();
}

void exec(sqlite3* db, const char* sql) {
    check(sqlite3_exec(db, sql, nullptr, nullptr, nullptr));
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql) {
        check(sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr));
    }
    ~Statement() { sqlite3_finalize(statement_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void text(int i, std::string_view value) {
        check(sqlite3_bind_text(statement_, i, value.empty() ? "" : value.data(),
                                static_cast<int>(value.size()), SQLITE_TRANSIENT));
    }

    void number(int i, std::uint64_t value) {
        check(sqlite3_bind_int64(statement_, i, static_cast<sqlite3_int64>(value)));
    }

    void key(const CacheKey& key) {
        text(1, key.media.provider);
        text(2, key.media.account);
        text(3, key.media.value);
        text(4, key.revision);
    }

    void bytes(int i, std::span<const std::uint8_t> value) {
        check(sqlite3_bind_blob(statement_, i, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT));
    }

    bool row() {
        const int result = sqlite3_step(statement_);
        if (result == SQLITE_ROW) return true;
        if (result == SQLITE_DONE) return false;
        throw DiskError();
    }

    std::uint64_t integer(int i) const {
        return static_cast<std::uint64_t>(sqlite3_column_int64(statement_, i));
    }

    std::span<const std::uint8_t> blob(int i) const {
        return {static_cast<const std::uint8_t*>(sqlite3_column_blob(statement_, i)),
                static_cast<std::size_t>(sqlite3_column_bytes(statement_, i))};
    }

private:
    sqlite3_stmt* statement_ = nullptr;
};

struct Transaction {
    explicit Transaction(sqlite3* connection) : db(connection) { exec(db, "BEGIN IMMEDIATE"); }
    ~Transaction() { if (!committed) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); }
    void commit() { exec(db, "COMMIT"); committed = true; }
    sqlite3* db;
    bool committed = false;
};

// Detect damaged payloads separately from SQLite's page structure checks.
std::uint32_t checksum(std::span<const std::uint8_t> bytes) {
    std::uint32_t hash = 2166136261u;
    for (const auto byte : bytes) hash = (hash ^ byte) * 16777619u;
    return hash;
}

struct Scope {
    bool valid = true; // Access under the database mutex.
};

using Account = std::pair<std::string, std::string>;

} // namespace

struct DiskCache::Impl : std::enable_shared_from_this<Impl> {
    struct Source;
    struct Lease;
    Impl(const std::filesystem::path&, DiskCacheLimits);
    std::uint64_t usage(std::string_view provider = {}, std::string_view account = {}) const;
    std::vector<std::uint8_t> get(const CacheKey&, const Scope&, std::uint64_t, std::size_t);
    void store(const CacheKey&, const Scope&, std::uint64_t, std::span<const std::uint8_t>);
    bool room(std::uint64_t, std::uint64_t, std::string_view = {}, std::string_view = {});
    void purge(std::string_view, std::string_view);
    void checkScope(const Scope& scope) const {
        if (!scope.valid) throw ReadError(BG_VIDEO_RANGE_CANCELLED);
    }

    std::mutex mutex;
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db{nullptr, sqlite3_close};
    DiskCacheLimits limits;
    std::map<Account, std::weak_ptr<Scope>> scopes;
    std::uint64_t tick = 0;
    std::uint64_t reused = 0;
};

DiskCache::Impl::Impl(const std::filesystem::path& directory, DiskCacheLimits budget) : limits(budget) {
    if (directory.empty() || limits.totalBytes > std::numeric_limits<std::int64_t>::max())
        throw std::invalid_argument("invalid video cache directory or quota");
    std::filesystem::create_directories(directory);
    const auto path = (directory / "bg-video-cache-v1.sqlite").u8string();
    sqlite3* connection = nullptr;
    const int result = sqlite3_open_v2(reinterpret_cast<const char*>(path.c_str()), &connection,
                                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_PRIVATECACHE,
                                       nullptr);
    db.reset(connection);
    check(result);
    check(sqlite3_busy_timeout(db.get(), 100));
    exec(db.get(), "PRAGMA locking_mode=EXCLUSIVE; PRAGMA journal_mode=DELETE;"
                   "PRAGMA synchronous=FULL; PRAGMA auto_vacuum=FULL; PRAGMA secure_delete=ON;");
    Transaction transaction(db.get());
    exec(db.get(), R"sql(
        CREATE TABLE IF NOT EXISTS block(
          provider TEXT NOT NULL, account TEXT NOT NULL, item TEXT NOT NULL, revision TEXT NOT NULL,
          offset INTEGER NOT NULL, payload BLOB NOT NULL, checksum INTEGER NOT NULL, used INTEGER NOT NULL,
          UNIQUE(provider, account, item, revision, offset)
        );
        CREATE INDEX IF NOT EXISTS block_lru ON block(used);
        CREATE TEMP TABLE pin(
          provider TEXT NOT NULL, account TEXT NOT NULL, item TEXT NOT NULL, revision TEXT NOT NULL,
          refs INTEGER NOT NULL, PRIMARY KEY(provider, account, item, revision)
        ) WITHOUT ROWID;
    )sql");
    {
        Statement latest(db.get(), "SELECT coalesce(max(used),0) FROM block");
        if (latest.row()) tick = latest.integer(0);
    }
    // Reapply quotas after reopening with a smaller configuration.
    std::vector<Account> accounts;
    {
        Statement account(db.get(), "SELECT DISTINCT provider,account FROM block");
        while (account.row()) {
            const auto provider = account.blob(0);
            const auto name = account.blob(1);
            accounts.emplace_back(std::string(provider.begin(), provider.end()),
                                  std::string(name.begin(), name.end()));
        }
    }
    for (const auto& [provider, account] : accounts) {
        room(0, limits.accountBytes, provider, account);
        room(0, limits.providerBytes, provider);
    }
    room(0, limits.totalBytes);
    transaction.commit();
}

std::uint64_t DiskCache::Impl::usage(std::string_view provider, std::string_view account) const {
    Statement query(db.get(), "SELECT coalesce(sum(length(payload)),0) FROM block "
                              "WHERE (?1='' OR provider=?1) AND (?2='' OR account=?2)");
    query.text(1, provider);
    query.text(2, account);
    return query.row() ? query.integer(0) : 0;
}

std::vector<std::uint8_t> DiskCache::Impl::get(
    const CacheKey& key, const Scope& scope, std::uint64_t offset, std::size_t expected) {
    std::lock_guard lock(mutex);
    checkScope(scope);
    std::vector<std::uint8_t> bytes;
    std::uint64_t rowId = 0;
    {
        Statement query(db.get(), "SELECT rowid, payload, checksum FROM block "
            "WHERE provider=?1 AND account=?2 AND item=?3 AND revision=?4 AND offset=?5");
        query.key(key);
        query.number(5, offset);
        if (!query.row()) return {};
        rowId = query.integer(0);
        const auto value = query.blob(1);
        if (value.size() == expected && checksum(value) == query.integer(2))
            bytes.assign(value.begin(), value.end());
    }
    if (bytes.empty()) {
        Statement remove(db.get(), "DELETE FROM block WHERE rowid=?1");
        remove.number(1, rowId);
        remove.row();
    } else {
        Statement touch(db.get(), "UPDATE block SET used=?1 WHERE rowid=?2");
        touch.number(1, ++tick);
        touch.number(2, rowId);
        touch.row();
    }
    return bytes;
}

bool DiskCache::Impl::room(std::uint64_t incoming, std::uint64_t quota,
                           std::string_view provider, std::string_view account) {
    if (incoming > quota) return false;
    auto used = usage(provider, account);
    while (used > quota - incoming) {
        Statement discard(db.get(), R"sql(
          DELETE FROM block WHERE rowid=(
            SELECT rowid FROM block AS b
            WHERE (?1='' OR b.provider=?1) AND (?2='' OR b.account=?2)
              AND NOT EXISTS(
                SELECT 1 FROM pin AS p WHERE p.provider=b.provider AND p.account=b.account
                  AND p.item=b.item AND p.revision=b.revision
              )
            ORDER BY used LIMIT 1
          ) RETURNING length(payload)
        )sql");
        discard.text(1, provider);
        discard.text(2, account);
        if (!discard.row()) return false;
        used -= discard.integer(0);
        discard.row();
    }
    return true;
}

void DiskCache::Impl::store(const CacheKey& key, const Scope& scope, std::uint64_t offset,
                            std::span<const std::uint8_t> bytes) {
    std::lock_guard lock(mutex);
    checkScope(scope);
    Transaction transaction(db.get());
    {
        Statement exists(db.get(), "SELECT 1 FROM block WHERE provider=?1 AND account=?2 "
                                    "AND item=?3 AND revision=?4 AND offset=?5");
        exists.key(key);
        exists.number(5, offset);
        if (exists.row()) return;
    }
    if (!room(bytes.size(), limits.accountBytes, key.media.provider, key.media.account) ||
        !room(bytes.size(), limits.providerBytes, key.media.provider) ||
        !room(bytes.size(), limits.totalBytes)) return;
    Statement insert(db.get(), "INSERT INTO block(provider,account,item,revision,offset,payload,checksum,used)"
                              " VALUES(?1,?2,?3,?4,?5,?6,?7,?8)");
    insert.key(key);
    insert.number(5, offset);
    insert.bytes(6, bytes);
    insert.number(7, checksum(bytes));
    insert.number(8, ++tick);
    insert.row();
    transaction.commit();
}

void DiskCache::Impl::purge(std::string_view provider, std::string_view account) {
    std::lock_guard lock(mutex);
    for (auto it = scopes.begin(); it != scopes.end();) {
        if (provider.empty() || (it->first.first == provider && it->first.second == account)) {
            if (const auto scope = it->second.lock()) scope->valid = false;
            it = scopes.erase(it);
        } else ++it;
    }
    Transaction transaction(db.get());
    Statement blocks(db.get(), "DELETE FROM block WHERE ?1='' OR (provider=?1 AND account=?2)");
    Statement pins(db.get(), "DELETE FROM pin WHERE ?1='' OR (provider=?1 AND account=?2)");
    for (auto* statement : {&blocks, &pins}) {
        statement->text(1, provider);
        statement->text(2, account);
        statement->row();
    }
    transaction.commit();
}

struct DiskCache::Impl::Lease {
    std::shared_ptr<Impl> cache;
    std::shared_ptr<Scope> scope;
    CacheKey key;
    bool attached = false;

    ~Lease() {
        if (!attached) return;
        try {
            std::lock_guard lock(cache->mutex);
            if (!scope->valid) return;
            const auto db = cache->db.get();
            Statement decrement(db, "UPDATE pin SET refs=refs-1 WHERE provider=?1 AND account=?2 "
                                    "AND item=?3 AND revision=?4");
            decrement.key(key);
            decrement.row();
            exec(db, "DELETE FROM pin WHERE refs=0");
        } catch (const DiskError&) {}
    }
};

struct DiskCache::Impl::Source final : ByteSource {
    Source(std::shared_ptr<Impl> owner, std::shared_ptr<Scope> permit, CacheKey identity,
           std::shared_ptr<ByteSource> origin)
        : cache(std::move(owner)), scope(std::move(permit)), key(std::move(identity)), input(std::move(origin)) {}

    std::uint64_t size() const noexcept override { return input->size(); }

    std::shared_ptr<void> pin() override {
        auto lease = std::make_shared<Lease>();
        lease->cache = cache;
        lease->scope = scope;
        lease->key = key;
        std::lock_guard lock(cache->mutex);
        cache->checkScope(*scope);
        Statement insert(cache->db.get(), "INSERT INTO pin VALUES(?1,?2,?3,?4,1) "
                                         "ON CONFLICT DO UPDATE SET refs=refs+1");
        insert.key(key);
        insert.row();
        lease->attached = true;
        return lease;
    }

    void readAt(std::uint64_t offset, std::span<std::uint8_t> output, const ReadRequest& request) override {
        if (offset > size() || output.size() > size() - offset) throw ReadError(BG_VIDEO_RANGE_IO);
        while (!output.empty()) {
            checkRead(request);
            const auto block = offset - offset % blockSize;
            const auto length = static_cast<std::size_t>(std::min(blockSize, size() - block));
            std::vector<std::uint8_t> bytes;
            try { bytes = cache->get(key, *scope, block, length); }
            catch (const DiskError&) {}
            const bool hit = !bytes.empty();
            if (bytes.empty()) {
                bytes.resize(length);
                input->readAt(block, bytes, request);
                checkRead(request);
                try { cache->store(key, *scope, block, bytes); }
                catch (const DiskError&) {}
            }
            const auto skip = static_cast<std::size_t>(offset - block);
            const auto count = std::min(output.size(), bytes.size() - skip);
            {
                std::lock_guard lock(cache->mutex);
                cache->checkScope(*scope);
                checkRead(request);
                std::copy_n(bytes.data() + skip, count, output.data());
                if (hit) cache->reused += std::min<std::uint64_t>(count, std::numeric_limits<std::uint64_t>::max() - cache->reused);
            }
            output = output.subspan(count);
            offset += count;
        }
    }

    std::shared_ptr<Impl> cache;
    std::shared_ptr<Scope> scope;
    CacheKey key;
    std::shared_ptr<ByteSource> input;
};

DiskCache::DiskCache(const std::filesystem::path& directory, DiskCacheLimits limits)
    : impl_(std::make_shared<Impl>(directory, limits)) {}
DiskCache::~DiskCache() = default;

std::shared_ptr<ByteSource> DiskCache::wrap(
    CacheKey key, DiskPolicy policy, std::shared_ptr<ByteSource> source) {
    if (!source) throw std::invalid_argument("media source is null");
    if (policy == DiskPolicy::noStore) return source;
    for (const auto* field : {&key.media.provider, &key.media.account, &key.media.value, &key.revision}) {
        if (field->empty() || field->size() > 512 || field->find('\0') != std::string::npos)
            throw std::invalid_argument("invalid video cache key");
    }
    if (source->size() > std::numeric_limits<std::int64_t>::max())
        throw std::invalid_argument("invalid video cache source length");
    std::lock_guard lock(impl_->mutex);
    auto& weak = impl_->scopes[{key.media.provider, key.media.account}];
    auto scope = weak.lock();
    if (!scope) weak = scope = std::make_shared<Scope>();
    return std::make_shared<Impl::Source>(impl_, std::move(scope), std::move(key), std::move(source));
}

void DiskCache::clearAccount(const std::string& provider, const std::string& account) {
    if (provider.empty() || account.empty() || provider.size() > 512 || account.size() > 512 ||
        provider.find('\0') != std::string::npos || account.find('\0') != std::string::npos)
        throw std::invalid_argument("invalid cache account");
    impl_->purge(provider, account);
}

void DiskCache::clear() { impl_->purge({}, {}); }

std::uint64_t DiskCache::storedBytes() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->usage();
}

std::uint64_t DiskCache::reusedBytes() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->reused;
}

}
