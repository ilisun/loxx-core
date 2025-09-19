#include "pbf_reader.h"

#include <cmath>
#include <stdexcept>
#include <unordered_map>

#ifdef HAVE_LIBOSMIUM
#  include <osmium/io/any_input.hpp>
#  include <osmium/handler.hpp>
#  include <osmium/visitor.hpp>
#  include <osmium/osm/types.hpp>
#  include <osmium/osm/way.hpp>
#  include <osmium/osm/node.hpp>
#endif

PbfReader::PbfReader(std::string input_path, int zoom)
  : input_path_(std::move(input_path)), zoom_(zoom) {}

static inline long long make_key(const TileKey& k) {
  return (static_cast<long long>(k.z) << 58) ^ (static_cast<long long>(k.x) << 29) ^ static_cast<long long>(k.y);
}

std::unordered_map<long long, TileData> PbfReader::readAndTile() {
  std::unordered_map<long long, TileData> result;

#ifdef HAVE_LIBOSMIUM
  osmium::io::Reader reader{input_path_};

  std::unordered_map<osmium::object_id_type, SimpleNode> node_index;

  // Первый проход: собрать узлы
  while (osmium::memory::Buffer buffer = reader.read()) {
    for (const osmium::OSMEntity& entity : buffer) {
      if (entity.type() == osmium::item_type::node) {
        const auto& n = static_cast<const osmium::Node&>(entity);
        SimpleNode sn{n.id(), n.location().lat(), n.location().lon()};
        node_index[sn.id] = sn;
      }
    }
  }
  reader.close();

  // Второй проход: собрать ways c highway=*
  osmium::io::Reader reader2{input_path_};
  while (osmium::memory::Buffer buffer = reader2.read()) {
    for (const osmium::OSMEntity& entity : buffer) {
      if (entity.type() == osmium::item_type::way) {
        const auto& w = static_cast<const osmium::Way&>(entity);
        const char* highway = w.tags().get_value_by_key("highway");
        if (!highway) continue;

        // Простейший маппинг классов
        int road_class = 3; // RESIDENTIAL default
        if (std::strcmp(highway, "motorway") == 0) road_class = 0;
        else if (std::strcmp(highway, "primary") == 0) road_class = 1;
        else if (std::strcmp(highway, "secondary") == 0) road_class = 2;
        else if (std::strcmp(highway, "footway") == 0) road_class = 4;
        else if (std::strcmp(highway, "path") == 0) road_class = 5;
        else if (std::strcmp(highway, "steps") == 0) road_class = 6;

        const bool oneway = w.tags().has_tag("oneway", "yes");

        std::vector<SimpleNode> shape;
        shape.reserve(w.nodes().size());
        for (const auto& nd_ref : w.nodes()) {
          auto it = node_index.find(nd_ref.positive_ref());
          if (it == node_index.end()) continue;
          shape.push_back(it->second);
        }
        if (shape.size() < 2) continue;

        // Разложить по сегментам между последовательными точками
        for (size_t s = 1; s < shape.size(); ++s) {
          const SimpleNode& a = shape[s - 1];
          const SimpleNode& b = shape[s];
          // Тайл по центру сегмента
          const double lat_c = 0.5 * (a.lat + b.lat);
          const double lon_c = 0.5 * (a.lon + b.lon);
          TileKey tk = tileKeyFor(lat_c, lon_c, zoom_);
          long long key = make_key(tk);
          auto& td = result[key];
          td.key = tk;
          td.bbox = tileBounds(tk);

          // Добавить вершины (уникальность по id в будущем, пока просто пушим)
          td.nodes.push_back(a);
          td.nodes.push_back(b);

          SimpleEdge e;
          e.from_node_id = a.id;
          e.to_node_id = b.id;
          e.shape = {a, b};
          e.oneway = oneway;
          e.road_class = road_class;
          e.car_access = !(road_class >= 4); // нет для чисто пешеходных
          e.foot_access = true;
          td.edges.push_back(std::move(e));
        }
      }
    }
  }
  reader2.close();
#else
  (void)result; // подавить предупреждения в окружении без libosmium
#endif

  return result;
}


