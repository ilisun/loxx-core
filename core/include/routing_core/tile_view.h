#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "land_tile_generated.h"

namespace routing_core {

class TileView {
public:
  explicit TileView(std::shared_ptr<std::vector<uint8_t>> buffer)
      : buffer_(std::move(buffer)) {
    root_ = flatbuffers::GetRoot<Routing::LandTile>(buffer_->data());
  }

  inline int nodeCount() const {
    return root_->nodes() ? static_cast<int>(root_->nodes()->size()) : 0;
  }
  inline int edgeCount() const {
    return root_->edges() ? static_cast<int>(root_->edges()->size()) : 0;
  }
  inline bool valid() const { return root_ != nullptr; }

  inline double nodeLat(int idx) const {
    const auto* n = root_->nodes()->Get(static_cast<flatbuffers::uoffset_t>(idx));
    return static_cast<double>(n->lat_q()) / 1e6;
  }
  inline double nodeLon(int idx) const {
    const auto* n = root_->nodes()->Get(static_cast<flatbuffers::uoffset_t>(idx));
    return static_cast<double>(n->lon_q()) / 1e6;
  }

  inline const Routing::Edge* edge(int idx) const {
    return root_->edges()->Get(static_cast<flatbuffers::uoffset_t>(idx));
  }

  inline const Routing::LandTile* root() const { return root_; }

private:
  std::shared_ptr<std::vector<uint8_t>> buffer_;
  const Routing::LandTile* root_ {nullptr};
};

} // namespace routing_core


