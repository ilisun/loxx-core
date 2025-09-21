#include <cstdio>
#include <vector>
#include <string>
#include "routing_core/router.h"

using namespace routing_core;

int main(int argc, char** argv) {
  if (argc < 6) {
    std::fprintf(stderr, "Usage: %s routingdb lat1 lon1 lat2 lon2\n", argv[0]);
    return 1;
  }
  std::string db = argv[1];
  Coord a{std::stod(argv[2]), std::stod(argv[3])};
  Coord b{std::stod(argv[4]), std::stod(argv[5])};

  Router r(db);
  auto res = r.route(Profile::Car, {a, b});
  if (res.status != RouteStatus::OK) {
    const char* st = "";
    switch (res.status) {
      case RouteStatus::NO_ROUTE: st = "NO_ROUTE"; break;
      case RouteStatus::NO_TILE: st = "NO_TILE"; break;
      case RouteStatus::DATA_ERROR: st = "DATA_ERROR"; break;
      case RouteStatus::INTERNAL_ERROR: st = "INTERNAL_ERROR"; break;
      default: st = ""; break;
    }
    std::fprintf(stderr, "Route failed: %s %s\n", st, res.error_message.c_str());
    return 2;
  }
  std::printf("distance_m=%.2f duration_s=%.2f points=%zu\n", res.distance_m, res.duration_s, res.polyline.size());
  for (const auto& p : res.polyline) {
    std::printf("%.6f %.6f\n", p.lat, p.lon);
  }
  return 0;
}


