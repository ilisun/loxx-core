#pragma once

#include <sqlite3.h>
#include <string>
#include <unordered_map>
#include <memory>
#include <vector>
#include <list>

namespace routing_core {

struct TileKey { int z; int x; int y; };
inline bool operator==(const TileKey& a, const TileKey& b) {
  return a.z==b.z && a.x==b.x && a.y==b.y;
}
struct TileKeyHash {
  size_t operator()(const TileKey& k) const {
    return (static_cast<size_t>(k.z) << 28) ^ (static_cast<size_t>(k.x) << 14) ^ static_cast<size_t>(k.y);
  }
};

struct TileBlob {
  TileKey key;
  std::shared_ptr<std::vector<uint8_t>> buffer; // владеем памятью
};

class TileStore {
public:
  TileStore(const std::string& db_path, size_t cacheCapacity);
  ~TileStore();

  // Загружает BLOB тайла по ключу (LRU-кэш). nullptr при отсутствии.
  std::shared_ptr<TileBlob> load(int z, int x, int y);

  int zoom() const { return zoom_; }
  void setZoom(int z) { zoom_ = z; }

private:
  // LRU
  using ListIt = std::list<TileKey>::iterator;
  struct CacheEntry {
    std::shared_ptr<TileBlob> blob;
    ListIt it;
  };

  std::shared_ptr<TileBlob> loadFromDb(int z, int x, int y);
  void insertLRU(const TileKey& key, std::shared_ptr<TileBlob> blob);

private:
  sqlite3* db_ {nullptr};
  int zoom_ {14};

  size_t capacity_;
  std::list<TileKey> lru_; // front = most recent
  std::unordered_map<TileKey, CacheEntry, TileKeyHash> map_;
};

} // namespace routing_core
