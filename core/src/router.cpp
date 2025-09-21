#include "routing_core/router.h"
#include "routing_core/tile_store.h"

#include <flatbuffers/flatbuffers.h>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <queue>
#include <algorithm>

#include "land_tile_generated.h"
#include "routing_core/tile_view.h"
#include "routing_core/tiler.h"

namespace routing_core {

struct Router::Impl {
  TileStore store;
  explicit Impl(const std::string& db) : store(db) {}

  static double haversine(double lat1, double lon1, double lat2, double lon2) {
    constexpr double R = 6371000.0;
    const double p1 = lat1 * M_PI/180.0;
    const double p2 = lat2 * M_PI/180.0;
    const double dphi = (lat2 - lat1) * M_PI/180.0;
    const double dl = (lon2 - lon1) * M_PI/180.0;
    const double a = std::sin(dphi/2)*std::sin(dphi/2) + std::cos(p1)*std::cos(p2)*std::sin(dl/2)*std::sin(dl/2);
    const double c = 2 * std::atan2(std::sqrt(a), std::sqrt(1-a));
    return R * c;
  }
  static int findClosestNode(const TileView& view, double lat, double lon) {
    int best = -1; double best_d = 1e100;
    for (int i = 0; i < view.nodeCount(); ++i) {
      double d = haversine(lat, lon, view.nodeLat(i), view.nodeLon(i));
      if (d < best_d) { best_d = d; best = i; }
    }
    return best;
  }
};

Router::Router(const std::string& db_path) : impl_(new Impl(db_path)) {}

RouteResult Router::route(Profile profile, const std::vector<Coord>& waypoints) {
  RouteResult rr;
  if (waypoints.size() < 2) {
    rr.status = RouteStatus::INTERNAL_ERROR;
    rr.error_message = "need at least 2 waypoints";
    return rr;
  }

  // Найдём тайл старта
  auto start_key = webTileKeyFor(waypoints.front().lat, waypoints.front().lon, 14);
  auto blob = impl_->store.load(start_key.z, start_key.x, start_key.y);
  if (!blob) { rr.status = RouteStatus::NO_TILE; rr.error_message = "no tile for start point"; return rr; }
  TileView view(blob->buffer);
  if (!view.valid() || view.edgeCount() == 0 || view.nodeCount() < 2) { rr.status = RouteStatus::NO_ROUTE; rr.error_message = "empty tile or no edges"; return rr; }

  // Снап старт/финиш к ближайшим узлам (в пределах одного тайла для v1)
  int s = Impl::findClosestNode(view, waypoints.front().lat, waypoints.front().lon);
  int t = Impl::findClosestNode(view, waypoints.back().lat, waypoints.back().lon);
  if (s < 0 || t < 0) { rr.status = RouteStatus::NO_ROUTE; rr.error_message = "failed to snap to nodes"; return rr; }

  // bi-A* (упрощённый Dijkstra/A* без встречного фронта для v1 внутри тайла)
  const int N = view.nodeCount();
  std::vector<double> dist(N, 1e100);
  std::vector<int> prev(N, -1);
  struct QNode { int v; double f; };
  struct Cmp { bool operator()(const QNode& a, const QNode& b) const { return a.f > b.f; } };
  std::priority_queue<QNode, std::vector<QNode>, Cmp> pq;
  dist[s] = 0.0;
  pq.push({s, 0.0});

  auto edge_relax = [&](int from) {
    // edges are per-tile; scan all edges and relax those that start at 'from'
    for (int ei = 0; ei < view.edgeCount(); ++ei) {
      const auto* e = view.edge(ei);
      int u = static_cast<int>(e->from_node());
      int v = static_cast<int>(e->to_node());
      if (u != from) continue;
      double length = e->length_m();
      double speed = (profile == Profile::Car) ? e->speed_mps() : e->foot_speed_mps();
      if (speed <= 0.0) continue;
      double w = length / speed;
      if (dist[u] + w < dist[v]) {
        dist[v] = dist[u] + w;
        prev[v] = u;
        double h = Impl::haversine(view.nodeLat(v), view.nodeLon(v), view.nodeLat(t), view.nodeLon(t)) / ((profile == Profile::Car) ? 13.9 : 1.4);
        pq.push({v, dist[v] + h});
      }
    }
  };

  while (!pq.empty()) {
    auto cur = pq.top(); pq.pop();
    if (cur.v == t) break;
    edge_relax(cur.v);
  }

  if (prev[t] == -1 && s != t) { rr.status = RouteStatus::NO_ROUTE; rr.error_message = "no path within tile"; return rr; }

  // Восстановить путь по prev → polyline
  std::vector<int> path;
  for (int v = t; v != -1; v = prev[v]) path.push_back(v);
  std::reverse(path.begin(), path.end());
  rr.polyline.clear();
  rr.distance_m = 0.0;
  for (size_t i = 0; i < path.size(); ++i) {
    rr.polyline.push_back(Coord{view.nodeLat(path[i]), view.nodeLon(path[i])});
    if (i > 0) rr.distance_m += Impl::haversine(view.nodeLat(path[i-1]), view.nodeLon(path[i-1]), view.nodeLat(path[i]), view.nodeLon(path[i]));
  }
  rr.duration_s = dist[t];
  rr.status = RouteStatus::OK;
  return rr;
}

} // namespace routing_core


