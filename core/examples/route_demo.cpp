#include <cstdio>
#include <vector>
#include <string>
#include <cstdlib>

#include "routing_core/router.h"
#include "routing_core/profile.h"
#include "routing_core/tiler.h"
#include "routing_core/tile_store.h"
#include "routing_core/tile_view.h"

using namespace routing_core;

int main(int argc, char** argv) {
  if (argc < 6) {
    std::fprintf(stderr,
      "Usage: %s routingdb lat1 lon1 lat2 lon2 [profile] [--dump]\n"
      "profile: car|foot (default car)\n"
      "--dump  : dump info about tile edges\n",
      argv[0]);
    return 1;
  }
  std::string db = argv[1];
  Coord a{std::stod(argv[2]), std::stod(argv[3])};
  Coord b{std::stod(argv[4]), std::stod(argv[5])};
  auto profile = makeCarProfile();
  bool dump = false;
  int zoomOpt = 14;
  // простенький парсер дополнительных флагов
  for (int i = 6; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "car") profile = makeCarProfile();
    else if (arg == "foot") profile = makeFootProfile();
    else if (arg == "--dump") dump = true;
    else if (arg == "--z" && i+1 < argc) { zoomOpt = std::atoi(argv[++i]); }
  }

  RouterOptions opt;
  opt.tileZoom = zoomOpt;     // можно менять
  opt.tileCacheCapacity = 128;
  Router r(db, opt);

  // Покажем тайлы обеих точек
  auto keyA = webTileKeyFor(a.lat, a.lon, opt.tileZoom);
  auto keyB = webTileKeyFor(b.lat, b.lon, opt.tileZoom);
  std::fprintf(stderr, "Point A tile z=%d x=%d y=%d\n",
               keyA.z, keyA.x, keyA.y);
  std::fprintf(stderr, "Point B tile z=%d x=%d y=%d\n",
               keyB.z, keyB.x, keyB.y);

  // Загрузим тайл старта для проверки
  TileStore store(db, 1);
  auto blob = store.load(keyA.z, keyA.x, keyA.y);
  if (blob) {
    TileView view(blob->buffer);
    std::fprintf(stderr, "Tile nodes=%d edges=%d\n",
                 view.nodeCount(), view.edgeCount());
    if (dump) {
      for (int ei = 0; ei < view.edgeCount(); ++ei) {
        auto* e = view.edgeAt(ei);
        std::fprintf(stderr,
          "edge %d from=%u to=%u len=%.1fm speed=%.1fm/s foot=%.1fm/s access_mask=%u oneway=%d\n",
          ei, e->from_node(), e->to_node(),
          e->length_m(), e->speed_mps(), e->foot_speed_mps(),
          e->access_mask(), e->oneway());
      }
    }
  } else {
    std::fprintf(stderr, "No tile blob for A\n");
  }

  // Вызываем роутер
  auto res = r.route(profile, {a, b});
  if (res.status != RouteStatus::OK) {
    const char* st = "";
    switch (res.status) {
      case RouteStatus::NO_ROUTE: st = "NO_ROUTE"; break;
      case RouteStatus::NO_TILE: st = "NO_TILE"; break;
      case RouteStatus::DATA_ERROR: st = "DATA_ERROR"; break;
      case RouteStatus::INTERNAL_ERROR: st = "INTERNAL_ERROR"; break;
      case RouteStatus::OK: st = "OK"; break;
    }
    std::fprintf(stderr, "Route failed: %s (%s)\n",
                 st, res.error_message.c_str());
    return 2;
  }

  std::printf("distance_m=%.2f duration_s=%.2f points=%zu edges=%zu\n",
              res.distance_m, res.duration_s,
              res.polyline.size(), res.edge_ids.size());
  for (const auto& p : res.polyline) {
    std::printf("%.6f %.6f\n", p.lat, p.lon);
  }
  return 0;
}
