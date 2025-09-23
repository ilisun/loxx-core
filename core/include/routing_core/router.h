#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>

namespace routing_core {

enum class Profile { Car, Foot };

enum class RouteStatus {
  OK,
  NO_ROUTE,
  NO_TILE,
  DATA_ERROR,
  INTERNAL_ERROR
};

struct Coord {
  double lat{};
  double lon{};
};

struct RouteResult {
  RouteStatus status {RouteStatus::INTERNAL_ERROR};
  std::vector<Coord> polyline;        // геометрия маршрута
  double distance_m {0.0};            // суммарная длина
  double duration_s {0.0};            // суммарное время (сек)
  std::vector<uint64_t> edge_ids;     // идентификаторы реальных рёбер маршрута (без виртуальных)
  std::string error_message;          // описание ошибки (опц.)
};

struct RouterOptions {
  int tileZoom = 14;                  // уровень тайла (совпадает с конвертером)
  size_t tileCacheCapacity = 128;     // LRU-кэш тайлов
};

class Router {
public:
  explicit Router(const std::string& db_path, RouterOptions opt = {});
  ~Router();

  // Маршрут через start..waypoints..end (в v1 — все точки в одном тайле)
  RouteResult route(Profile profile, const std::vector<Coord>& waypoints);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace routing_core
