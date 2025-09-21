#pragma once

#include <sqlite3.h>
#include <string>
#include <unordered_map>
#include <memory>
#include <vector>

namespace routing_core {

struct TileKey { int z; int x; int y; };

struct TileBlob {
  TileKey key;
  std::shared_ptr<std::vector<uint8_t>> buffer; // владеем памятью
};

class TileStore {
public:
  explicit TileStore(const std::string& db_path);
  ~TileStore();

  // Загружает BLOB тайла по ключу. Возвращает nullptr при отсутствии.
  std::unique_ptr<TileBlob> load(int z, int x, int y);

private:
  sqlite3* db_ {nullptr};
};

} // namespace routing_core


