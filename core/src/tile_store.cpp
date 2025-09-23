#include "routing_core/tile_store.h"

#include <stdexcept>
#include <cstring>

using namespace routing_core;

TileStore::TileStore(const std::string& db_path, size_t cacheCapacity)
  : capacity_(cacheCapacity) {
  if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
    throw std::runtime_error(std::string("Failed to open routingdb: ") + sqlite3_errmsg(db_));
  }
  sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
  sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
  sqlite3_exec(db_, "PRAGMA temp_store=MEMORY;", nullptr, nullptr, nullptr);
}

TileStore::~TileStore() {
  if (db_) sqlite3_close(db_);
}

std::shared_ptr<TileBlob> TileStore::load(int z, int x, int y) {
  TileKey key{z,x,y};
  auto it = map_.find(key);
  if (it != map_.end()) {
    // move to front
    lru_.erase(it->second.it);
    lru_.push_front(key);
    it->second.it = lru_.begin();
    return it->second.blob;
  }

  auto blob = loadFromDb(z,x,y);
  if (!blob) return nullptr;
  insertLRU(key, blob);
  return blob;
}

std::shared_ptr<TileBlob> TileStore::loadFromDb(int z, int x, int y) {
  static const char* sql =
      "SELECT data FROM land_tiles WHERE z=? AND x=? AND y=? LIMIT 1;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return nullptr;
  }
  sqlite3_bind_int(stmt, 1, z);
  sqlite3_bind_int(stmt, 2, x);
  sqlite3_bind_int(stmt, 3, y);

  std::shared_ptr<TileBlob> out;
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    const void* blob = sqlite3_column_blob(stmt, 0);
    int size = sqlite3_column_bytes(stmt, 0);
    if (blob && size > 0) {
      auto vec = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(size));
      std::memcpy(vec->data(), blob, static_cast<size_t>(size));
      out = std::make_shared<TileBlob>();
      out->key = TileKey{z, x, y};
      out->buffer = std::move(vec);
    }
  }
  sqlite3_finalize(stmt);
  return out;
}

void TileStore::insertLRU(const TileKey& key, std::shared_ptr<TileBlob> blob) {
  if (capacity_ == 0) return;
  if (lru_.size() >= capacity_) {
    auto last = lru_.back();
    lru_.pop_back();
    map_.erase(last);
  }
  lru_.push_front(key);
  map_[key] = CacheEntry{std::move(blob), lru_.begin()};
}
