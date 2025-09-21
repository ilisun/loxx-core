#include "routing_core/tile_store.h"

#include <stdexcept>
#include <cstring>

using namespace routing_core;

TileStore::TileStore(const std::string& db_path) {
  if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
    throw std::runtime_error("Failed to open routingdb");
  }
  sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
}

TileStore::~TileStore() {
  if (db_) sqlite3_close(db_);
}

std::unique_ptr<TileBlob> TileStore::load(int z, int x, int y) {
  static const char* sql =
      "SELECT data FROM land_tiles WHERE z=? AND x=? AND y=? LIMIT 1;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return nullptr;
  }
  sqlite3_bind_int(stmt, 1, z);
  sqlite3_bind_int(stmt, 2, x);
  sqlite3_bind_int(stmt, 3, y);
  std::unique_ptr<TileBlob> out;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const void* blob = sqlite3_column_blob(stmt, 0);
    int size = sqlite3_column_bytes(stmt, 0);
    if (blob && size > 0) {
      auto vec = std::make_shared<std::vector<uint8_t>>();
      vec->resize(size);
      std::memcpy(vec->data(), blob, size);
      out = std::make_unique<TileBlob>();
      out->key = TileKey{z, x, y};
      out->buffer = std::move(vec);
    }
  }
  sqlite3_finalize(stmt);
  return out;
}


