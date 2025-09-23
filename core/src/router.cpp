#include "routing_core/router.h"
#include "routing_core/tile_store.h"

#include <flatbuffers/flatbuffers.h>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <queue>
#include <algorithm>
#include <limits>
#include <optional>

#include "land_tile_generated.h"
#include "routing_core/tile_view.h"
#include "routing_core/tiler.h"

namespace routing_core {

struct Router::Impl {
  TileStore store;
  int tileZoom;

  explicit Impl(const std::string& db, const RouterOptions& opt)
    : store(db, opt.tileCacheCapacity), tileZoom(opt.tileZoom) {
    store.setZoom(tileZoom);
  }

  // --- геодезия ---
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

  // --- снап к ребру ---
  struct EdgeSnap {
    uint32_t edgeIdx{0};
    int fromNode{-1};
    int toNode{-1};
    int segIndex{-1};           // индекс сегмента внутри формы (если форма есть)
    double t{0.0};              // параметр проекции на сегмент [0..1]
    double projLat{0.0};
    double projLon{0.0};
    double dist_m{std::numeric_limits<double>::infinity()};
  };

  static void projectPointToSegment(double ax, double ay, double bx, double by,
                                    double px, double py, double& outX, double& outY, double& t) {
    // проекция в евклидовой метрике; для малых сегментов ок
    const double vx = bx - ax, vy = by - ay;
    const double wx = px - ax, wy = py - ay;
    double c1 = vx*wx + vy*wy;
    double c2 = vx*vx + vy*vy;
    t = (c2 <= 1e-12) ? 0.0 : std::max(0.0, std::min(1.0, c1 / c2));
    outX = ax + t*vx;
    outY = ay + t*vy;
  }

  static std::optional<EdgeSnap> snapToEdge(const TileView& view, double lat, double lon) {
    if (!view.valid() || view.edgeCount() == 0) return std::nullopt;
    EdgeSnap best;
    bool has = false;

    std::vector<std::pair<double,double>> tmp;
    tmp.reserve(64);

    for (int ei = 0; ei < view.edgeCount(); ++ei) {
      tmp.clear();
      view.appendEdgeShape(static_cast<uint32_t>(ei), tmp, /*skipFirst*/false);
      if (tmp.size() < 2) continue;
      for (int k = 0; k+1 < static_cast<int>(tmp.size()); ++k) {
        // Работать в плоскости (lon=x, lat=y), затем обратно
        double projLon, projLat, t;
        projectPointToSegment(
          /*ax=*/tmp[k].second,      /*ay=*/tmp[k].first,
          /*bx=*/tmp[k+1].second,    /*by=*/tmp[k+1].first,
          /*px=*/lon,                /*py=*/lat,
          /*outX=*/projLon,          /*outY=*/projLat,
          t);
        double d = haversine(lat, lon, projLat, projLon);
        if (d < best.dist_m) {
          has = true;
          best.edgeIdx = static_cast<uint32_t>(ei);
          const auto* e = view.edgeAt(best.edgeIdx);
          best.fromNode = static_cast<int>(e->from_node());
          best.toNode   = static_cast<int>(e->to_node());
          best.segIndex = k;
          best.t = t;
          best.projLat = projLat;
          best.projLon = projLon;
          best.dist_m = d;
        }
      }
    }
    if (!has) return std::nullopt;
    return best;
  }

  // --- утилиты доступа/веса ---
  static bool edgeAllowed(const Routing::Edge* e, Profile profile, int fromNode) {
    auto mask = e->access_mask();
    bool carOk  = (profile == Profile::Car)  && (mask & 1);
    bool footOk = (profile == Profile::Foot) && (mask & 2);
    if (!carOk && !footOk) return false;
    bool oneway = e->oneway();
    if (oneway && fromNode != static_cast<int>(e->from_node())) return false;
    return true;
  }

  static double edgeTraversalTimeSec(const Routing::Edge* e, Profile profile) {
    double speed = (profile == Profile::Car) ? e->speed_mps() : e->foot_speed_mps();
    if (speed <= 0.0) return std::numeric_limits<double>::infinity();
    return e->length_m() / speed;
  }

  // --- виртуальные рёбра/узлы для снапа ---
  struct VirtualEdge {
    int from{-1};
    int to{-1};
    double length_m{0.0};
    double duration_s{0.0};
    uint32_t access_mask{0};
    bool oneway{false};
    // простая геометрия (две точки), чтобы собрать polyline
    Coord a{}, b{};
    // индекс реального ребра в тайле, к которому относится виртуальный сегмент
    int realEdgeIdx{-1};
  };

  // bi-A* внутри одного тайла + виртуальные узлы
  RouteResult routeSingleTile(Profile profile,
                              const TileKey& key,
                              TileView& view,
                              const EdgeSnap& startSnap,
                              const EdgeSnap& endSnap) {
    RouteResult rr;

    const int N = view.nodeCount();
    if (N < 2 || view.edgeCount() == 0) {
      rr.status = RouteStatus::NO_ROUTE;
      rr.error_message = "empty tile";
      return rr;
    }

    // Построим виртуальные узлы
    const int vStart = N;     // виртуальный узел старта
    const int vEnd   = N + 1; // виртуальный узел финиша
    const int VN     = N + 2; // общее число узлов в вычислении

    auto* eStart = view.edgeAt(startSnap.edgeIdx);
    auto* eEnd   = view.edgeAt(endSnap.edgeIdx);

    auto speedOf = [&](const Routing::Edge* e)->double {
      return (profile == Profile::Car) ? e->speed_mps() : e->foot_speed_mps();
    };

    // длины/времена долей ребра
    auto lenStart  = eStart->length_m();
    auto durStart  = (speedOf(eStart) > 0.0) ? (lenStart / speedOf(eStart)) : std::numeric_limits<double>::infinity();
    auto tS = std::clamp(startSnap.t, 0.0, 1.0);

    auto lenEnd    = eEnd->length_m();
    auto durEnd    = (speedOf(eEnd) > 0.0) ? (lenEnd / speedOf(eEnd)) : std::numeric_limits<double>::infinity();
    auto tE = std::clamp(endSnap.t, 0.0, 1.0);

    // Виртуальные рёбра для старта:
    // 1) from -> vStart  (доля t)
    // 2) vStart -> to    (доля 1-t)
    VirtualEdge vs1{
      startSnap.fromNode, vStart,
      lenStart * tS,
      durStart * tS,
      eStart->access_mask(),
      eStart->oneway(),
      {view.nodeLat(startSnap.fromNode), view.nodeLon(startSnap.fromNode)},
      {startSnap.projLat, startSnap.projLon},
      static_cast<int>(startSnap.edgeIdx)
    };
    VirtualEdge vs2{
      vStart, startSnap.toNode,
      lenStart * (1.0 - tS),
      durStart * (1.0 - tS),
      eStart->access_mask(),
      eStart->oneway(),
      {startSnap.projLat, startSnap.projLon},
      {view.nodeLat(startSnap.toNode), view.nodeLon(startSnap.toNode)},
      static_cast<int>(startSnap.edgeIdx)
    };

    // Виртуальные рёбра для финиша:
    // 1) from -> vEnd   (доля t)
    // 2) vEnd  -> to    (доля 1-t)
    VirtualEdge ve1{
      endSnap.fromNode, vEnd,
      lenEnd * tE,
      durEnd * tE,
      eEnd->access_mask(),
      eEnd->oneway(),
      {view.nodeLat(endSnap.fromNode), view.nodeLon(endSnap.fromNode)},
      {endSnap.projLat, endSnap.projLon},
      static_cast<int>(endSnap.edgeIdx)
    };
    VirtualEdge ve2{
      vEnd, endSnap.toNode,
      lenEnd * (1.0 - tE),
      durEnd * (1.0 - tE),
      eEnd->access_mask(),
      eEnd->oneway(),
      {endSnap.projLat, endSnap.projLon},
      {view.nodeLat(endSnap.toNode), view.nodeLon(endSnap.toNode)},
      static_cast<int>(endSnap.edgeIdx)
    };

    // Собираем список виртуальных рёбер (учитываем oneway: если oneway=true, vs1 допустим только from->vStart, a vStart->from не допускаем)
    std::vector<VirtualEdge> virt;
    virt.reserve(4);
    virt.push_back(vs1);
    virt.push_back(vs2);
    virt.push_back(ve1);
    virt.push_back(ve2);

    // Мелкая утилита доступа к «исходящим» виртуальным рёбрам из узла u
    auto virtOutEdges = [&](int u, std::vector<int>& outIdxs) {
      outIdxs.clear();
      for (int i = 0; i < static_cast<int>(virt.size()); ++i) {
        if (virt[i].from == u) outIdxs.push_back(i);
      }
    };
    // И входящие для обратного фронта
    auto virtInEdges = [&](int u, std::vector<int>& outIdxs) {
      outIdxs.clear();
      for (int i = 0; i < static_cast<int>(virt.size()); ++i) {
        if (virt[i].to == u) outIdxs.push_back(i);
      }
    };

    // --- bi-A* между vStart и vEnd ---
    struct QNode { int v; double f; };
    struct Cmp { bool operator()(const QNode& a, const QNode& b) const { return a.f > b.f; } };

    struct Label {
      double g{std::numeric_limits<double>::infinity()};
      int prevNode{-1};
      uint32_t prevEdge{std::numeric_limits<uint32_t>::max()}; // индекс реального ребра (если брали real-edge)
      int prevVirt{-1}; // индекс виртуального ребра (если брали виртуальный)
    };

    std::vector<Label> F(VN), B(VN);

    auto speedHeur = (profile == Profile::Car) ? 13.9 : 1.4;
    auto h = [&](int v, const Coord& target)->double {
      double lv = (v < N) ? view.nodeLat(v) : (v == vStart ? startSnap.projLat : endSnap.projLat);
      double lo = (v < N) ? view.nodeLon(v) : (v == vStart ? startSnap.projLon : endSnap.projLon);
      return haversine(lv, lo, target.lat, target.lon) / speedHeur;
    };

    std::priority_queue<QNode, std::vector<QNode>, Cmp> pqF, pqB;

    Coord targetF{endSnap.projLat, endSnap.projLon};
    Coord targetB{startSnap.projLat, startSnap.projLon};

    F[vStart].g = 0.0;
    pqF.push({vStart, h(vStart, targetF)});

    B[vEnd].g = 0.0;
    pqB.push({vEnd, h(vEnd, targetB)});

    double bestMu = std::numeric_limits<double>::infinity();
    int meet = -1;

    std::vector<int> tmpIdx;

    auto relaxForward = [&](int u) {
      // 1) реальные исходящие рёбра
      if (u < N) {
        uint32_t start = view.firstEdge(u);
        uint16_t cnt   = view.edgeCountFrom(u);
        for (uint32_t k = 0; k < cnt; ++k) {
          uint32_t ei = start + k;
          const auto* e = view.edgeAt(ei);
          if (!edgeAllowed(e, profile, u)) continue;
          int v = static_cast<int>(e->to_node());
          double w = edgeTraversalTimeSec(e, profile);
          if (!std::isfinite(w)) continue;
          double cand = F[u].g + w;
          if (cand < F[v].g) {
            F[v].g = cand;
            F[v].prevNode = u;
            F[v].prevEdge = ei;
            F[v].prevVirt = -1;
            pqF.push({v, cand + h(v, targetF)});
            if (B[v].g < std::numeric_limits<double>::infinity()) {
              double mu = cand + B[v].g;
              if (mu < bestMu) { bestMu = mu; meet = v; }
            }
          }
        }
      }
      // 2) виртуальные исходящие
      virtOutEdges(u, tmpIdx);
      for (int idx : tmpIdx) {
        const auto& e = virt[idx];
        // доступ/oneway — берём как есть, т.к. уже "направили" from→to
        if (e.duration_s == std::numeric_limits<double>::infinity()) continue;
        int v = e.to;
        double cand = F[u].g + e.duration_s;
        if (cand < F[v].g) {
          F[v].g = cand;
          F[v].prevNode = u;
          F[v].prevEdge = std::numeric_limits<uint32_t>::max();
          F[v].prevVirt = idx;
          pqF.push({v, cand + h(v, targetF)});
          if (B[v].g < std::numeric_limits<double>::infinity()) {
            double mu = cand + B[v].g;
            if (mu < bestMu) { bestMu = mu; meet = v; }
          }
        }
      }
    };

    auto relaxBackward = [&](int u) {
      // 1) реальные входящие рёбра
      if (u < N) {
        const auto& inE = view.inEdgesOf(u);
        for (auto ei : inE) {
          const auto* e = view.edgeAt(ei);
          int from = static_cast<int>(e->from_node());
          if (!edgeAllowed(e, profile, from)) continue;
          double w = edgeTraversalTimeSec(e, profile);
          if (!std::isfinite(w)) continue;
          double cand = B[u].g + w;
          if (cand < B[from].g) {
            B[from].g = cand;
            B[from].prevNode = u;
            B[from].prevEdge = ei;
            B[from].prevVirt = -1;
            pqB.push({from, cand + h(from, targetB)});
            if (F[from].g < std::numeric_limits<double>::infinity()) {
              double mu = cand + F[from].g;
              if (mu < bestMu) { bestMu = mu; meet = from; }
            }
          }
        }
      }
      // 2) виртуальные входящие
      virtInEdges(u, tmpIdx);
      for (int idx : tmpIdx) {
        const auto& e = virt[idx];
        if (e.duration_s == std::numeric_limits<double>::infinity()) continue;
        int from = e.from;
        double cand = B[u].g + e.duration_s;
        if (cand < B[from].g) {
          B[from].g = cand;
          B[from].prevNode = u;
          B[from].prevEdge = std::numeric_limits<uint32_t>::max();
          B[from].prevVirt = idx;
          pqB.push({from, cand + h(from, targetB)});
          if (F[from].g < std::numeric_limits<double>::infinity()) {
            double mu = cand + F[from].g;
            if (mu < bestMu) { bestMu = mu; meet = from; }
          }
        }
      }
    };

    // Основной цикл
    while (!pqF.empty() || !pqB.empty()) {
      if (!pqF.empty()) {
        auto q = pqF.top(); pqF.pop();
        if (F[q.v].g + h(q.v, targetF) > bestMu) break;
        relaxForward(q.v);
      }
      if (!pqB.empty()) {
        auto q = pqB.top(); pqB.pop();
        if (B[q.v].g + h(q.v, targetB) > bestMu) break;
        relaxBackward(q.v);
      }
    }

    if (meet < 0 && vStart != vEnd) {
      rr.status = RouteStatus::NO_ROUTE;
      rr.error_message = "no path within tile";
      return rr;
    }

    // Восстановление пути: vStart -> meet по F, meet -> vEnd по B
    std::vector<std::pair<bool,uint64_t>> used; // (isVirt, id/edgeIdx)
    auto pushForward = [&](int v) {
      while (v != vStart && v >= 0) {
        if (F[v].prevVirt >= 0) {
          used.emplace_back(true, static_cast<uint64_t>(F[v].prevVirt));
        } else {
          used.emplace_back(false, static_cast<uint64_t>(F[v].prevEdge));
        }
        v = F[v].prevNode;
      }
    };
    auto pushBackward = [&](int v) {
      while (v != vEnd && v >= 0) {
        if (B[v].prevVirt >= 0) {
          used.emplace_back(true, static_cast<uint64_t>(B[v].prevVirt));
        } else {
          used.emplace_back(false, static_cast<uint64_t>(B[v].prevEdge));
        }
        v = B[v].prevNode;
      }
    };

    pushForward(meet);
    std::reverse(used.begin(), used.end());
    pushBackward(meet);

    // Сборка polyline и метрик
    rr.polyline.clear();
    rr.edge_ids.clear();
    rr.distance_m = 0.0;
    rr.duration_s = 0.0;

    auto appendPoint = [&](double lat, double lon) {
      if (!rr.polyline.empty()) {
        auto& last = rr.polyline.back();
        if (last.lat == lat && last.lon == lon) return;
        rr.distance_m += haversine(last.lat, last.lon, lat, lon);
      }
      rr.polyline.push_back(Coord{lat, lon});
    };

    uint64_t lastEdgeIdPushed = std::numeric_limits<uint64_t>::max();
    for (auto [isVirt, id] : used) {
      if (isVirt) {
        const auto& e = virt[static_cast<int>(id)];
        // Виртуальные рёбра добавляем геометрией двух точек
        if (rr.polyline.empty()) appendPoint(e.a.lat, e.a.lon);
        else appendPoint(e.a.lat, e.a.lon); // возможно совпадёт и просто не добавится
        appendPoint(e.b.lat, e.b.lon);
        rr.duration_s += e.duration_s;
        // Добавим соответствующий реальный edgeId (избегая дублей подряд)
        if (e.realEdgeIdx >= 0) {
          uint64_t eid = makeEdgeId(key.z, key.x, key.y, static_cast<uint32_t>(e.realEdgeIdx));
          if (eid != lastEdgeIdPushed) {
            rr.edge_ids.push_back(eid);
            lastEdgeIdPushed = eid;
          }
        }
      } else {
        uint32_t ei = static_cast<uint32_t>(id);
        // Реальное ребро: добавляем его shape
        std::vector<std::pair<double,double>> pts;
        view.appendEdgeShape(ei, pts, /*skipFirst*/!rr.polyline.empty());
        for (auto& p : pts) appendPoint(p.first, p.second);
        rr.duration_s += edgeTraversalTimeSec(view.edgeAt(ei), profile);
        uint64_t eid = makeEdgeId(key.z, key.x, key.y, ei);
        if (eid != lastEdgeIdPushed) {
          rr.edge_ids.push_back(eid);
          lastEdgeIdPushed = eid;
        }
      }
    }

    rr.status = RouteStatus::OK;
    return rr;
  }

}; // Impl

Router::Router(const std::string& db_path, RouterOptions opt)
  : impl_(std::make_unique<Impl>(db_path, opt)) {}

Router::~Router() = default;

RouteResult Router::route(Profile profile, const std::vector<Coord>& waypoints) {
  RouteResult rr;
  if (waypoints.size() < 2) {
    rr.status = RouteStatus::INTERNAL_ERROR;
    rr.error_message = "need at least 2 waypoints";
    return rr;
  }

  // В v1 — все точки должны быть в одном тайле
  auto key0 = webTileKeyFor(waypoints.front().lat, waypoints.front().lon, impl_->tileZoom);
  for (size_t i=1;i<waypoints.size();++i) {
    auto ki = webTileKeyFor(waypoints[i].lat, waypoints[i].lon, impl_->tileZoom);
    if (ki.x != key0.x || ki.y != key0.y || ki.z != key0.z) {
      rr.status = RouteStatus::NO_ROUTE;
      rr.error_message = "multi-tile routing not supported yet (schema lacks cross-tile connectivity)";
      return rr;
    }
  }

  auto blob = impl_->store.load(key0.z, key0.x, key0.y);
  if (!blob) { rr.status = RouteStatus::NO_TILE; rr.error_message = "no tile for start"; return rr; }
  TileView view(blob->buffer);
  if (!view.valid() || view.edgeCount() == 0 || view.nodeCount() < 2) {
    rr.status = RouteStatus::NO_ROUTE; rr.error_message = "empty tile"; return rr;
  }

  RouteResult total;
  total.status = RouteStatus::OK;

  for (size_t i=0;i+1<waypoints.size();++i) {
    auto sSnap = Impl::snapToEdge(view, waypoints[i].lat,   waypoints[i].lon);
    auto tSnap = Impl::snapToEdge(view, waypoints[i+1].lat, waypoints[i+1].lon);
    if (!sSnap || !tSnap) {
      rr.status = RouteStatus::NO_ROUTE;
      rr.error_message = "failed to snap to edge";
      return rr;
    }

    auto segRes = impl_->routeSingleTile(profile, TileKey{key0.z,key0.x,key0.y}, view, *sSnap, *tSnap);
    if (segRes.status != RouteStatus::OK) return segRes;

    if (i == 0) {
      total = std::move(segRes);
    } else {
      // Склейка без дублирования
      if (!total.polyline.empty() && !segRes.polyline.empty()) {
        auto& last = total.polyline.back();
        auto& first = segRes.polyline.front();
        if (last.lat == first.lat && last.lon == first.lon) {
          // ок, просто сшиваем
        } else {
          total.polyline.push_back(first);
        }
      }
      total.polyline.insert(total.polyline.end(),
                            segRes.polyline.begin() + (segRes.polyline.empty()?0:1),
                            segRes.polyline.end());
      total.distance_m += segRes.distance_m; // segRes.distance_m уже посчитана в сборке polyline
      total.duration_s += segRes.duration_s;
      total.edge_ids.insert(total.edge_ids.end(), segRes.edge_ids.begin(), segRes.edge_ids.end());
    }
  }

  return total;
}

} // namespace routing_core
