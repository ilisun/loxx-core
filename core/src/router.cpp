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
#include <unordered_map>
#include <map>

#include "land_tile_generated.h"
#include "routing_core/tile_view.h"
#include "routing_core/tiler.h"
#include "routing_core/edge_id.h"
#include "routing_core/profile.h"

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

  // --- упаковка/распаковка edge_id: 64 бита = [z:8][x:20][y:20][ei:16]
  static uint64_t makeEdgeId(int z, uint32_t x, uint32_t y, uint32_t edgeIdx) { return edgeid::make(z,x,y,edgeIdx); }
  static void parseEdgeId(uint64_t id, int& z, uint32_t& x, uint32_t& y, uint32_t& edgeIdx) { edgeid::parse(id,z,x,y,edgeIdx); }

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

  static std::optional<EdgeSnap> snapToEdge(const TileView& view, double lat, double lon, const ProfileSettings& profile) {
    if (!view.valid() || view.edgeCount() == 0) return std::nullopt;
    EdgeSnap best;
    bool has = false;

    std::vector<std::pair<double,double>> tmp;
    tmp.reserve(64);

    for (int ei = 0; ei < view.edgeCount(); ++ei) {
      tmp.clear();
      const auto* e = view.edgeAt(static_cast<uint32_t>(ei));
      // профильная доступность: должна быть скорость > 0 и доступен профиль
      auto rc = static_cast<int>(e->road_class());
      double sp = profile.speeds_mps[rc];
      bool allowed = (profile.access_mask & e->access_mask()) != 0;
      if (!allowed || sp <= 0.0) continue;
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
  static bool edgeAllowed(const Routing::Edge* e, const ProfileSettings& profile, int fromNode) {
    if ((profile.access_mask & e->access_mask()) == 0) return false;
    bool oneway = e->oneway();
    if (oneway && fromNode != static_cast<int>(e->from_node())) return false;
    return true;
  }

  static double edgeTraversalTimeSec(const Routing::Edge* e, const ProfileSettings& profile) {
    auto rc = static_cast<int>(e->road_class());
    double speed = profile.speeds_mps[rc];
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
  RouteResult routeSingleTile(const ProfileSettings& profile,
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
      int rc = static_cast<int>(e->road_class());
      return profile.speeds_mps[rc];
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

    double speedHeur = 0.0;
    for (double v : profile.speeds_mps) { if (v > speedHeur) speedHeur = v; }
    if (speedHeur <= 0.0) speedHeur = 1.0;
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

  // ---- Мультитайловый граф (с коннекторами по lat_q/lon_q) ----
  struct GlobalEdge { int to; double w; uint8_t isVirt; uint32_t tileX, tileY, edgeIdx; };
  struct GlobalNode { double lat, lon; };

  // key for quantized coordinate
  struct QKey { int32_t lat_q; int32_t lon_q; };
  struct QKeyHash { size_t operator()(const QKey& k) const noexcept { return (static_cast<size_t>(static_cast<uint32_t>(k.lat_q))<<32) ^ static_cast<size_t>(static_cast<uint32_t>(k.lon_q)); } };
  struct QKeyEq { bool operator()(const QKey& a, const QKey& b) const noexcept { return a.lat_q==b.lat_q && a.lon_q==b.lon_q; } };

  // Сбор прямоугольника тайлов (с рамкой)
  void collectTileRange(const Coord& a, const Coord& b, int frame,
                        std::vector<TileKey>& out) {
    auto ka = webTileKeyFor(a.lat, a.lon, tileZoom);
    auto kb = webTileKeyFor(b.lat, b.lon, tileZoom);
    int minx = std::min(ka.x, kb.x) - frame;
    int maxx = std::max(ka.x, kb.x) + frame;
    int miny = std::min(ka.y, kb.y) - frame;
    int maxy = std::max(ka.y, kb.y) + frame;
    for (int y=miny;y<=maxy;++y) {
      for (int x=minx;x<=maxx;++x) {
        out.push_back(TileKey{tileZoom,x,y});
      }
    }
  }

  // Построение глобального графа из набора тайлов
  void buildGlobalGraph(const ProfileSettings& profile,
                        const std::vector<std::pair<TileKey,TileView>>& tiles,
                        std::vector<GlobalNode>& nodes,
                        std::vector<std::vector<GlobalEdge>>& adj,
                        std::vector<std::vector<std::pair<int,int>>>& revAdj,
                        std::unordered_map<uint64_t,int>& q2node) {
    nodes.clear(); adj.clear(); revAdj.clear(); q2node.clear();

    auto nodeIdFor = [&](int32_t lat_q, int32_t lon_q, double lat, double lon) {
      uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(lat_q))<<32) ^ static_cast<uint64_t>(static_cast<uint32_t>(lon_q));
      auto it = q2node.find(key);
      if (it!=q2node.end()) return it->second;
      int id = static_cast<int>(nodes.size());
      nodes.push_back(GlobalNode{lat,lon});
      adj.emplace_back();
      revAdj.emplace_back();
      q2node.emplace(key,id);
      return id;
    };

    for (auto& tv : tiles) {
      const auto& tref = tv.first; const auto& view = tv.second;
      int N = view.nodeCount(); int E = view.edgeCount();
      // предварительно создать все ноды
      std::vector<int> local2global(N, -1);
      for (int i=0;i<N;++i) {
        local2global[i] = nodeIdFor(view.nodeLatQ(i), view.nodeLonQ(i), view.nodeLat(i), view.nodeLon(i));
      }
      // добавить рёбра
      for (int ei=0; ei<E; ++ei) {
        const auto* e = view.edgeAt(static_cast<uint32_t>(ei));
        if (!edgeAllowed(e, profile, static_cast<int>(e->from_node()))) continue;
        double w = edgeTraversalTimeSec(e, profile);
        if (!std::isfinite(w)) continue;
        int u = local2global[static_cast<int>(e->from_node())];
        int v = local2global[static_cast<int>(e->to_node())];
        adj[u].push_back(GlobalEdge{v, w, 0u, static_cast<uint32_t>(tref.x), static_cast<uint32_t>(tref.y), static_cast<uint32_t>(ei)});
        revAdj[v].push_back({u, static_cast<int>(adj[u].size()-1)});
        // если не oneway — добавить обратное ребро
        if (!e->oneway()) {
          // обратный проход допустим только если профилю разрешено
          if (edgeAllowed(e, profile, static_cast<int>(e->to_node()))) {
            adj[v].push_back(GlobalEdge{u, w, 0u, static_cast<uint32_t>(tref.x), static_cast<uint32_t>(tref.y), static_cast<uint32_t>(ei)});
            revAdj[u].push_back({v, static_cast<int>(adj[v].size()-1)});
          }
        }
      }
    }
  }

  // bi-A* по глобальному графу
bool astarGlobalBi(const std::vector<GlobalNode>& nodes,
                     const std::vector<std::vector<GlobalEdge>>& adj,
                     const std::vector<std::vector<std::pair<int,int>>>& revAdj,
                     int s, int t,
                     std::vector<int>& meetPath, std::vector<uint64_t>& usedEdgeIds) {
    struct L { double g{std::numeric_limits<double>::infinity()}; int prev{-1}; int prevEdge{-1}; };
    struct Q { int v; double f; }; struct C { bool operator()(const Q&a,const Q&b)const{return a.f>b.f;}};
    std::vector<L> F(nodes.size()), B(nodes.size());
    std::priority_queue<Q,std::vector<Q>,C> pqF, pqB;
    auto hF=[&](int v){ return haversine(nodes[v].lat,nodes[v].lon,nodes[t].lat,nodes[t].lon)/13.9; };
    auto hB=[&](int v){ return haversine(nodes[v].lat,nodes[v].lon,nodes[s].lat,nodes[s].lon)/13.9; };
    F[s].g=0.0; B[t].g=0.0; pqF.push({s,hF(s)}); pqB.push({t,hB(t)});
    double bestMu = std::numeric_limits<double>::infinity(); int meet=-1;
    while(!pqF.empty() || !pqB.empty()){
      if(!pqF.empty()){
        auto q=pqF.top(); pqF.pop();
        if (F[q.v].g + hF(q.v) > bestMu) break;
        for(size_t i=0;i<adj[q.v].size();++i){ const auto& e=adj[q.v][i]; double cand=F[q.v].g+e.w; if(cand<F[e.to].g){ F[e.to].g=cand; F[e.to].prev=q.v; F[e.to].prevEdge=static_cast<int>(i); pqF.push({e.to, cand + hF(e.to)}); if (B[e.to].g< std::numeric_limits<double>::infinity()){ double mu=cand+B[e.to].g; if(mu<bestMu){ bestMu=mu; meet=e.to; } } } }
      }
      if(!pqB.empty()){
        auto q=pqB.top(); pqB.pop();
        if (B[q.v].g + hB(q.v) > bestMu) break;
        for(const auto& re : revAdj[q.v]){ int from=re.first; int idx=re.second; const auto& e=adj[from][static_cast<size_t>(idx)]; double cand=B[q.v].g + e.w; if(cand<B[from].g){ B[from].g=cand; B[from].prev=q.v; B[from].prevEdge=idx; pqB.push({from, cand + hB(from)}); if (F[from].g< std::numeric_limits<double>::infinity()){ double mu=cand+F[from].g; if(mu<bestMu){ bestMu=mu; meet=from; } } } }
      }
    }
    if (meet<0) return false;
    // Восстановление пути F: s->meet, B: meet->t
    std::vector<std::pair<int,int>> seq; // (u, edgeIdxInAdj)
    for(int v=meet; v!=s; v=F[v].prev){ int u=F[v].prev; seq.emplace_back(u, F[v].prevEdge); }
    std::reverse(seq.begin(), seq.end());
    for(int v=meet; v!=t; v=B[v].prev){ int u=v; int idx=B[u].prevEdge; seq.emplace_back(u, idx); // edge u->B[u].prev
    }
    usedEdgeIds.clear(); uint64_t lastE=std::numeric_limits<uint64_t>::max();
    for(auto& p: seq){ int u=p.first; int idx=p.second; if (idx<0) continue; const auto& ge=adj[u][static_cast<size_t>(idx)]; uint64_t id=makeEdgeId(tileZoom, ge.tileX, ge.tileY, ge.edgeIdx); if(id!=lastE){ usedEdgeIds.push_back(id); lastE=id; } }
    // meetPath возвращать не обязательно для polyline, но заполним
    meetPath.clear(); meetPath.push_back(s); meetPath.push_back(meet); meetPath.push_back(t);
    return true;
  }

}; // Impl

Router::Router(const std::string& db_path, RouterOptions opt)
  : impl_(std::make_unique<Impl>(db_path, opt)) {}

Router::~Router() = default;

RouteResult Router::route(const ProfileSettings& profile, const std::vector<Coord>& waypoints) {
  RouteResult rr;
  if (waypoints.size() < 2) {
    rr.status = RouteStatus::INTERNAL_ERROR;
    rr.error_message = "need at least 2 waypoints";
    return rr;
  }

  // Мультитайловая версия v1: прямоугольник тайлов + динамическая рамка по расстоянию
  double dist_m_straight = Impl::haversine(waypoints.front().lat, waypoints.front().lon,
                                           waypoints.back().lat,  waypoints.back().lon);
  double dist_km = dist_m_straight / 1000.0;
  // Эвристика: размер тайла ~4 км на экваторе; возьмём запас +1 и лимит сверху
  int dyn_frame = static_cast<int>(std::ceil(dist_km / 4.0)) + 1;
  if (dyn_frame < 1) dyn_frame = 1;
  if (dyn_frame > 8) dyn_frame = 8;
  std::vector<TileKey> trefs; impl_->collectTileRange(waypoints.front(), waypoints.back(), dyn_frame, trefs);
  std::vector<std::pair<TileKey,TileView>> tiles;
  tiles.reserve(trefs.size());
  for (auto& tr : trefs) {
    auto b = impl_->store.load(tr.z, tr.x, tr.y);
    if (!b) continue;
    TileView v(b->buffer);
    if (!v.valid() || v.edgeCount()==0 || v.nodeCount()<2) continue;
    tiles.emplace_back(tr, std::move(v));
  }
  if (tiles.empty()) { rr.status = RouteStatus::NO_TILE; rr.error_message = "no tiles in range"; return rr; }

  std::vector<Impl::GlobalNode> nodes; std::vector<std::vector<Impl::GlobalEdge>> adj; std::vector<std::vector<std::pair<int,int>>> revAdj; std::unordered_map<uint64_t,int> q2node;
  impl_->buildGlobalGraph(profile, tiles, nodes, adj, revAdj, q2node);

  // снап по всем тайлам: просто выберем ближайший edgeSnap среди tiles[0..]
  auto bestSnap = [&](const Coord& c){
    std::optional<Impl::EdgeSnap> best; double bestD=std::numeric_limits<double>::infinity(); int bestTile=-1;
    for (int i=0;i<(int)tiles.size();++i){ auto s=Impl::snapToEdge(tiles[i].second,c.lat,c.lon, profile); if(!s) continue; if(s->dist_m<bestD){ best= s; bestD=s->dist_m; bestTile=i; } }
    return std::tuple{best, bestTile}; };

  auto [sSnap, sTile] = bestSnap(waypoints.front());
  auto [tSnap, tTile] = bestSnap(waypoints.back());
  if (!sSnap || !tSnap) { rr.status=RouteStatus::NO_ROUTE; rr.error_message="failed to snap (multi-tile)"; return rr; }

  // глобальные узлы для старта/финиша — привяжем к ближайшим реальным узлам (from/to соответствующих рёбер)
  auto& sView = tiles[sTile].second; auto& tView = tiles[tTile].second;
  int sFrom = q2node[(static_cast<uint64_t>(static_cast<uint32_t>(sView.nodeLatQ(sSnap->fromNode)))<<32) ^ static_cast<uint64_t>(static_cast<uint32_t>(sView.nodeLonQ(sSnap->fromNode)))];
  int sTo   = q2node[(static_cast<uint64_t>(static_cast<uint32_t>(sView.nodeLatQ(sSnap->toNode)))<<32) ^ static_cast<uint64_t>(static_cast<uint32_t>(sView.nodeLonQ(sSnap->toNode)))];
  int tFrom = q2node[(static_cast<uint64_t>(static_cast<uint32_t>(tView.nodeLatQ(tSnap->fromNode)))<<32) ^ static_cast<uint64_t>(static_cast<uint32_t>(tView.nodeLonQ(tSnap->fromNode)))];
  int tTo   = q2node[(static_cast<uint64_t>(static_cast<uint32_t>(tView.nodeLatQ(tSnap->toNode)))<<32) ^ static_cast<uint64_t>(static_cast<uint32_t>(tView.nodeLonQ(tSnap->toNode)))];

  // выберем стартовый и конечный глобальные узлы как ближайшие из пары (from/to) по геометрии
  auto pickClosest = [&](int a, int b, const Coord& c){ double da=Impl::haversine(nodes[a].lat,nodes[a].lon,c.lat,c.lon); double db=Impl::haversine(nodes[b].lat,nodes[b].lon,c.lat,c.lon); return (da<=db)?a:b; };
  int sNode = pickClosest(sFrom, sTo, waypoints.front());
  int tNode = pickClosest(tFrom, tTo, waypoints.back());

  std::vector<int> gpath; std::vector<uint64_t> eids;
  // Добавим виртуальные узлы vS,vE и полу-рёбра до ближайших узлов snapped-ребёр с учётом oneway
  int vS = static_cast<int>(nodes.size());
  nodes.push_back(Impl::GlobalNode{sSnap->projLat, sSnap->projLon});
  adj.emplace_back();
  int vE = static_cast<int>(nodes.size());
  nodes.push_back(Impl::GlobalNode{tSnap->projLat, tSnap->projLon});
  adj.emplace_back();

  auto addVS = [&](const TileView& view, const Impl::EdgeSnap& snap){
    const auto* e = view.edgeAt(snap.edgeIdx);
    double speed = profile.speeds_mps[static_cast<int>(e->road_class())];
    if (speed<=0.0) return;
    double w = e->length_m()/speed;
    double t = std::clamp(snap.t, 0.0, 1.0);
    // fromNode -> vS (доля t)
    if (!e->oneway()) {
      adj[sNode].push_back(Impl::GlobalEdge{vS, t*w, 1u, 0,0,0});
    } else {
      // oneway: допускаем вход в vS только если направление from->to
      uint64_t kFrom = (static_cast<uint64_t>(static_cast<uint32_t>(view.nodeLatQ(snap.fromNode)))<<32) ^ static_cast<uint64_t>(static_cast<uint32_t>(view.nodeLonQ(snap.fromNode)));
      int fromGlobal = q2node[kFrom];
      if (fromGlobal==sNode) adj[sNode].push_back(Impl::GlobalEdge{vS, t*w, 1u, 0,0,0});
    }
    // vS -> toNode (доля 1-t) всегда по направлению ребра
    uint64_t kTo = (static_cast<uint64_t>(static_cast<uint32_t>(view.nodeLatQ(snap.toNode)))<<32) ^ static_cast<uint64_t>(static_cast<uint32_t>(view.nodeLonQ(snap.toNode)));
    int toGlobal = q2node[kTo];
    adj[vS].push_back(Impl::GlobalEdge{toGlobal, (1.0-t)*w, 1u, 0,0,0});
    // если не oneway — позволяем обратный ход vS->fromNode
    if (!e->oneway()) {
      uint64_t kFrom2 = (static_cast<uint64_t>(static_cast<uint32_t>(view.nodeLatQ(snap.fromNode)))<<32) ^ static_cast<uint64_t>(static_cast<uint32_t>(view.nodeLonQ(snap.fromNode)));
      int fromGlobal = q2node[kFrom2];
      adj[vS].push_back(Impl::GlobalEdge{fromGlobal, t*w, 1u, 0,0,0});
    }
  };

  auto addVE = [&](const TileView& view, const Impl::EdgeSnap& snap){
    const auto* e = view.edgeAt(snap.edgeIdx);
    double speed = profile.speeds_mps[static_cast<int>(e->road_class())];
    if (speed<=0.0) return;
    double w = e->length_m()/speed;
    double t = std::clamp(snap.t, 0.0, 1.0);
    // fromNode -> vE (доля t) по направлению ребра
    uint64_t kFrom3 = (static_cast<uint64_t>(static_cast<uint32_t>(view.nodeLatQ(snap.fromNode)))<<32) ^ static_cast<uint64_t>(static_cast<uint32_t>(view.nodeLonQ(snap.fromNode)));
    int fromGlobal = q2node[kFrom3];
    adj[fromGlobal].push_back(Impl::GlobalEdge{vE, t*w, 1u, 0,0,0});
    // если не oneway — toNode -> vE (доля 1-t)
    if (!e->oneway()) {
      uint64_t kTo2 = (static_cast<uint64_t>(static_cast<uint32_t>(view.nodeLatQ(snap.toNode)))<<32) ^ static_cast<uint64_t>(static_cast<uint32_t>(view.nodeLonQ(snap.toNode)));
      int toGlobal = q2node[kTo2];
      adj[toGlobal].push_back(Impl::GlobalEdge{vE, (1.0-t)*w, 1u, 0,0,0});
    }
  };

  addVS(sView, *sSnap);
  addVE(tView, *tSnap);

  if (!impl_->astarGlobalBi(nodes, adj, revAdj, vS, vE, gpath, eids)) { rr.status=RouteStatus::NO_ROUTE; rr.error_message="no path in multi-tile"; return rr; }

  // собрать polyline по edgeIds
  rr.polyline.clear(); rr.edge_ids = eids; rr.distance_m=0; rr.duration_s=0;
  auto appendPoint=[&](double la,double lo){ if(!rr.polyline.empty()){ auto& L=rr.polyline.back(); rr.distance_m+=Impl::haversine(L.lat,L.lon,la,lo);} rr.polyline.push_back(Coord{la,lo}); };
  for (auto id : eids){
    int z; uint32_t x,y,ei; Impl::parseEdgeId(id, z, x, y, ei);
    // найдём view по (x,y)
    TileView const* vptr=nullptr;
    for (auto& pr: tiles){ if (pr.first.x==(int)x && pr.first.y==(int)y){ vptr=&pr.second; break; } }
    if (!vptr) continue;
    std::vector<std::pair<double,double>> pts;
    vptr->appendEdgeShape(static_cast<uint32_t>(ei), pts, /*skipFirst*/!rr.polyline.empty());
    for (auto& p:pts) appendPoint(p.first,p.second);
    rr.duration_s += Impl::edgeTraversalTimeSec(vptr->edgeAt(static_cast<uint32_t>(ei)), profile);
  }
  rr.status = RouteStatus::OK;
  return rr;
}

} // namespace routing_core
