#pragma once

#include <array>
#include <cstdint>

#include "land_tile_generated.h"

namespace routing_core {

struct ProfileSettings {
  uint16_t access_mask {0};
  std::array<double, static_cast<int>(Routing::RoadClass::STEPS)+1> speeds_mps {};
};

inline ProfileSettings makeCarProfile() {
  ProfileSettings p;
  p.access_mask = 1; // cars
  auto& s = p.speeds_mps;
  s[static_cast<int>(Routing::RoadClass::MOTORWAY)]    = 30.0;
  s[static_cast<int>(Routing::RoadClass::PRIMARY)]     = 25.0;
  s[static_cast<int>(Routing::RoadClass::SECONDARY)]   = 20.0;
  s[static_cast<int>(Routing::RoadClass::RESIDENTIAL)] = 15.0;
  s[static_cast<int>(Routing::RoadClass::FOOTWAY)]     = 1.0;
  s[static_cast<int>(Routing::RoadClass::PATH)]        = 1.0;
  s[static_cast<int>(Routing::RoadClass::STEPS)]       = 1.0;
  return p;
}

inline ProfileSettings makeFootProfile() {
  ProfileSettings p;
  p.access_mask = 2; // foot
  auto& s = p.speeds_mps;
  s[static_cast<int>(Routing::RoadClass::MOTORWAY)]    = 0.0;
  s[static_cast<int>(Routing::RoadClass::PRIMARY)]     = 0.0;
  s[static_cast<int>(Routing::RoadClass::SECONDARY)]   = 0.0;
  s[static_cast<int>(Routing::RoadClass::RESIDENTIAL)] = 1.4;
  s[static_cast<int>(Routing::RoadClass::FOOTWAY)]     = 1.4;
  s[static_cast<int>(Routing::RoadClass::PATH)]        = 1.4;
  s[static_cast<int>(Routing::RoadClass::STEPS)]       = 1.0;
  return p;
}

} // namespace routing_core


