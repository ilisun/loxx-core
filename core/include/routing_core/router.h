#pragma once

#include <string>
#include <vector>
#include <cstdint>

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
  double lat;
  double lon;
};

struct RouteResult {
  RouteStatus status {RouteStatus::INTERNAL_ERROR};
  std::vector<Coord> polyline;
  double distance_m {0.0};
  double duration_s {0.0};
  std::vector<uint64_t> edge_ids;
  std::string error_message;
};

class Router {
public:
  explicit Router(const std::string& db_path);
  RouteResult route(Profile profile, const std::vector<Coord>& waypoints);

private:
  struct Impl;
  Impl* impl_;
};

} // namespace routing_core


