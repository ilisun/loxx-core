#pragma once

#include <cmath>

namespace routing_core {

struct WebTileKey { int z; int x; int y; };

inline WebTileKey webTileKeyFor(double lat_deg, double lon_deg, int z) {
  const double lat_rad = lat_deg * M_PI / 180.0;
  const int n = 1 << z;
  int x = static_cast<int>(std::floor((lon_deg + 180.0) / 360.0 * n));
  int y = static_cast<int>(std::floor((1.0 - std::log(std::tan(lat_rad) + 1.0 / std::cos(lat_rad)) / M_PI) / 2.0 * n));
  if (x < 0) x = 0; if (x >= n) x = n - 1;
  if (y < 0) y = 0; if (y >= n) y = n - 1;
  return {z, x, y};
}

} // namespace routing_core


