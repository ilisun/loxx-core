#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <optional>

#include "land_tile_generated.h"

namespace routing_core {

// Обёртка для FlatBuffers-тайла с ленивыми индексами входящих рёбер.
class TileView {
public:
  explicit TileView(std::shared_ptr<std::vector<uint8_t>> buffer)
      : buffer_(std::move(buffer)) {
    root_ = flatbuffers::GetRoot<Routing::LandTile>(buffer_->data());
  }

  inline bool valid() const { return root_ != nullptr; }

  // Размеры
  inline int nodeCount() const {
    return root_->nodes() ? static_cast<int>(root_->nodes()->size()) : 0;
  }
  inline int edgeCount() const {
    return root_->edges() ? static_cast<int>(root_->edges()->size()) : 0;
  }

  // Координаты узла (квантованные в схеме)
  inline double nodeLat(int idx) const {
    const auto* n = root_->nodes()->Get(static_cast<flatbuffers::uoffset_t>(idx));
    return static_cast<double>(n->lat_q()) / 1e6;
  }
  inline double nodeLon(int idx) const {
    const auto* n = root_->nodes()->Get(static_cast<flatbuffers::uoffset_t>(idx));
    return static_cast<double>(n->lon_q()) / 1e6;
  }

  // Смежность (out-edges) из узла
  inline uint32_t firstEdge(int nodeIdx) const {
    const auto* n = root_->nodes()->Get(static_cast<flatbuffers::uoffset_t>(nodeIdx));
    return n->first_edge();
  }
  inline uint16_t edgeCountFrom(int nodeIdx) const {
    const auto* n = root_->nodes()->Get(static_cast<flatbuffers::uoffset_t>(nodeIdx));
    return n->edge_count();
  }
  inline const Routing::Edge* edgeAt(uint32_t edgeIdx) const {
    return root_->edges()->Get(static_cast<flatbuffers::uoffset_t>(edgeIdx));
  }

  // Входящие рёбра (для обратного фронта bi-A*)
  const std::vector<uint32_t>& inEdgesOf(int nodeIdx) const {
    ensureInAdjBuilt();
    return (*inAdj_)[static_cast<size_t>(nodeIdx)];
  }

  // Геометрия ребра: получить shape-точки
  void appendEdgeShape(uint32_t edgeIdx,
                       std::vector<std::pair<double,double>>& out,
                       bool skipFirst=true) const {
    const auto* e = edgeAt(edgeIdx);
    if (root_->shapes() && e->shape_count() > 0) {
      auto start = e->shape_start();
      auto cnt   = e->shape_count();
      for (uint32_t k = 0; k < cnt; ++k) {
        auto* sp = root_->shapes()->Get(static_cast<flatbuffers::uoffset_t>(start + k));
        if (skipFirst && k == 0 && !out.empty()) continue;
        out.emplace_back(static_cast<double>(sp->lat_q())/1e6,
                         static_cast<double>(sp->lon_q())/1e6);
      }
      return;
    }
    if (e->encoded_polyline()) {
      decodeEncodedPolyline(e->encoded_polyline()->str(), out, skipFirst);
      return;
    }
    // fallback: from→to
    int from = static_cast<int>(e->from_node());
    int to   = static_cast<int>(e->to_node());
    if (!skipFirst || out.empty()) out.emplace_back(nodeLat(from), nodeLon(from));
    out.emplace_back(nodeLat(to), nodeLon(to));
  }

  const Routing::LandTile* root() const { return root_; }

private:
  static void decodeEncodedPolyline(const std::string& s,
                                    std::vector<std::pair<double,double>>& out,
                                    bool skipFirst) {
    int index = 0, len = static_cast<int>(s.size());
    int lat = 0, lon = 0;
    bool first = true;
    auto next = [&](int& idx)->int {
      int result = 0, shift = 0, b = 0;
      do { b = s[idx++] - 63; result |= (b & 0x1f) << shift; shift += 5; } while (b >= 0x20 && idx < len);
      return ((result & 1) ? ~(result >> 1) : (result >> 1));
    };
    while (index < len) {
      lat += next(index);
      lon += next(index);
      double dlat = lat * 1e-5;
      double dlon = lon * 1e-5;
      if (skipFirst && first && !out.empty()) {
        // пропускаем первую точку, чтобы не дублировать
      } else {
        out.emplace_back(dlat, dlon);
      }
      first = false;
    }
  }

  void ensureInAdjBuilt() const {
    if (inAdj_) return;
    size_t N = static_cast<size_t>(nodeCount());
    inAdj_ = std::make_unique<std::vector<std::vector<uint32_t>>>(N);
    int E = edgeCount();
    for (int ei = 0; ei < E; ++ei) {
      const auto* e = edgeAt(static_cast<uint32_t>(ei));
      auto to = static_cast<size_t>(e->to_node());
      if (to < N) {
        (*inAdj_)[to].push_back(static_cast<uint32_t>(ei));
      }
    }
  }

private:
  std::shared_ptr<std::vector<uint8_t>> buffer_;
  const Routing::LandTile* root_ {nullptr};
  mutable std::unique_ptr<std::vector<std::vector<uint32_t>>> inAdj_;
};

} // namespace routing_core
