#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <filesystem>
#include <vector>
#if __APPLE__
#  include <CommonCrypto/CommonDigest.h>
#endif

#include "sqlite_writer.h"
#include "pbf_reader.h"
#include "serializer.h"

namespace fs = std::filesystem;

static void printUsage(const char* argv0) {
  std::fprintf(stderr, "Usage: %s [--z ZOOM] input.osm.pbf output.routingdb\n", argv0);
}

int main(int argc, char** argv) {
  if (argc < 3) {
    printUsage(argv[0]);
    return 1;
  }

  int zoom = 14;
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
  for (size_t i = 0; i < args.size();) {
    if (args[i] == "--z") {
      if (i + 1 >= args.size()) { printUsage(argv[0]); return 1; }
      zoom = std::stoi(args[i + 1]);
      args.erase(args.begin() + i, args.begin() + i + 2);
    } else {
      ++i;
    }
  }
  if (args.size() < 2) { printUsage(argv[0]); return 1; }

  const std::string inputPbfPath = args[0];
  const std::string outputDbPath = args[1];

  try {
    // Ensure output directory exists
    const fs::path outPath(outputDbPath);
    if (outPath.has_parent_path()) {
      fs::create_directories(outPath.parent_path());
    }
    if (fs::exists(outPath)) {
      fs::remove(outPath);
    }

    RoutingDbWriter writer(outputDbPath);
    writer.createSchemaIfNeeded();

    PbfReader reader(inputPbfPath, zoom);
    auto tiles = reader.readAndTile();

    // Пока только пишем metadata, чтобы DB был валиден
    writer.writeMetadata("schema_version", "1");
    writer.writeMetadata("source", inputPbfPath);

    std::printf("Parsed tiles: %zu\n", tiles.size());
    // Serialize and write
    const uint32_t version = 1;
    const uint32_t profile_mask = 0x3; // car|foot
    int count_written = 0;
    for (auto& [key, t] : tiles) {
      auto blob = buildLandTileBlob(t, version, profile_mask);

      // checksum
      std::string checksum_hex;
#if __APPLE__
      unsigned char digest[CC_SHA256_DIGEST_LENGTH];
      CC_SHA256(blob.data(), static_cast<CC_LONG>(blob.size()), digest);
      static const char* hex = "0123456789abcdef";
      checksum_hex.resize(CC_SHA256_DIGEST_LENGTH * 2);
      for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i) {
        checksum_hex[2*i] = hex[(digest[i] >> 4) & 0xF];
        checksum_hex[2*i+1] = hex[digest[i] & 0xF];
      }
#else
      checksum_hex = "";
#endif

      // Use real WebMercator z/x/y
      int z = t.key.z;
      int x = t.key.x;
      int y = t.key.y;

      writer.insertLandTile(z, x, y, t.bbox, version, checksum_hex, profile_mask,
                            blob.data(), blob.size());
      ++count_written;
    }
    std::printf("Written tiles: %d\n", count_written);
    std::puts("Created routing SQLite container with schema (metadata + land_tiles)");
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "Error: %s\n", ex.what());
    return 2;
  }
}


