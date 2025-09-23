#include <flatbuffers/flatbuffers.h>
#include <sqlite3.h>
#include <iostream>
#include <vector>
#include <cstdint>
#include <cmath>

#include "land_tile_generated.h"

using namespace Routing;

static void fail(const char* msg, sqlite3* db = nullptr) {
    std::cerr << msg;
    if (db) std::cerr << " : " << sqlite3_errmsg(db);
    std::cerr << "\n";
    if (db) sqlite3_close(db);
    std::exit(1);
}

// простая функция вычисления WebMercator tile
static void calcTileKey(double lat_deg, double lon_deg, int z, int &x, int &y) {
    double lat_rad = lat_deg * M_PI / 180.0;
    int n = 1 << z;
    x = (int)std::floor((lon_deg + 180.0) / 360.0 * n);
    y = (int)std::floor((1.0 - std::log(std::tan(lat_rad) + 1.0 / std::cos(lat_rad)) / M_PI) / 2.0 * n);
    if (x < 0) x = 0; if (x >= n) x = n - 1;
    if (y < 0) y = 0; if (y >= n) y = n - 1;
}

int main() {
    // Базовая координата, вокруг которой строим узлы
    double base_lat = 47.000000;
    double base_lon = 9.000000;
    int z = 14;
    int x, y;
    calcTileKey(base_lat, base_lon, z, x, y);

    std::cout << "Generating tile z=" << z << " x=" << x << " y=" << y << "\n";

    flatbuffers::FlatBufferBuilder fbb(1024);

    // Узлы (lat_q, затем lon_q)
    std::vector<flatbuffers::Offset<Node>> nodes_vec;
    auto lat_q_0 = static_cast<int32_t>(std::lround(47.000000 * 1e6));
    auto lon_q_0 = static_cast<int32_t>(std::lround(9.000000  * 1e6));
    auto lat_q_1 = static_cast<int32_t>(std::lround(47.000000 * 1e6));
    auto lon_q_1 = static_cast<int32_t>(std::lround(9.001000  * 1e6));
    auto lat_q_2 = static_cast<int32_t>(std::lround(47.000000 * 1e6));
    auto lon_q_2 = static_cast<int32_t>(std::lround(9.002000  * 1e6));

    nodes_vec.push_back(CreateNode(fbb, 0, lat_q_0, lon_q_0, 0, 1));
    nodes_vec.push_back(CreateNode(fbb, 1, lat_q_1, lon_q_1, 1, 1));
    nodes_vec.push_back(CreateNode(fbb, 2, lat_q_2, lon_q_2, 0, 0));
    auto nodes = fbb.CreateVector(nodes_vec);

    // Рёбра
    std::vector<flatbuffers::Offset<Edge>> edges_vec;
    edges_vec.push_back(CreateEdge(
        fbb, 0, 1,
        80.0f,
        13.9f, 1.4f,
        false,
        RoadClass::RESIDENTIAL,
        3, 0, 0, 0
    ));
    edges_vec.push_back(CreateEdge(
        fbb, 1, 2,
        80.0f,
        13.9f, 1.4f,
        false,
        RoadClass::RESIDENTIAL,
        3, 0, 0, 0
    ));
    auto edges = fbb.CreateVector(edges_vec);

    auto shapes = flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<ShapePoint>>>{};
    auto checksum = fbb.CreateString("");
    uint32_t version = 1;
    uint32_t profile_mask = 3;

    auto tile = CreateLandTile(
        fbb,
        z, x, y,
        nodes,
        edges,
        shapes,
        version,
        checksum,
        profile_mask
    );

    FinishLandTileBuffer(fbb, tile);
    const void* buf = fbb.GetBufferPointer();
    const int   sz  = (int)fbb.GetSize();

    // Верификация: распарсим буфер и выведем первые узлы (lat, lon)
    auto root = flatbuffers::GetRoot<LandTile>(buf);
    if (root && root->nodes() && root->nodes()->size() >= 2) {
        const auto* n0 = root->nodes()->Get(0);
        const auto* n1 = root->nodes()->Get(1);
        double n0_lat = static_cast<double>(n0->lat_q()) / 1e6;
        double n0_lon = static_cast<double>(n0->lon_q()) / 1e6;
        double n1_lat = static_cast<double>(n1->lat_q()) / 1e6;
        double n1_lon = static_cast<double>(n1->lon_q()) / 1e6;
        std::cout << "Node0 lat=" << n0_lat << " lon=" << n0_lon << "\n";
        std::cout << "Node1 lat=" << n1_lat << " lon=" << n1_lon << "\n";
    }

    // SQLite
    sqlite3* db = nullptr;
    if (sqlite3_open("test.routingdb", &db) != SQLITE_OK) {
        fail("Can't open DB", db);
    }

    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);

    const char* sql_create =
        "CREATE TABLE IF NOT EXISTS land_tiles ("
        "  z INT, x INT, y INT, data BLOB,"
        "  PRIMARY KEY (z,x,y)"
        ");";
    if (sqlite3_exec(db, sql_create, nullptr, nullptr, nullptr) != SQLITE_OK) {
        fail("Create table failed", db);
    }

    // удаляем возможный старый тайл
    {
        sqlite3_stmt* del = nullptr;
        const char* del_sql = "DELETE FROM land_tiles WHERE z=? AND x=? AND y=?;";
        if (sqlite3_prepare_v2(db, del_sql, -1, &del, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(del, 1, z);
            sqlite3_bind_int(del, 2, x);
            sqlite3_bind_int(del, 3, y);
            sqlite3_step(del);
            sqlite3_finalize(del);
        }
    }

    sqlite3_stmt* ins = nullptr;
    const char* ins_sql = "INSERT INTO land_tiles(z,x,y,data) VALUES (?,?,?,?);";
    if (sqlite3_prepare_v2(db, ins_sql, -1, &ins, nullptr) != SQLITE_OK) {
        fail("Prepare insert failed", db);
    }
    sqlite3_bind_int(ins, 1, z);
    sqlite3_bind_int(ins, 2, x);
    sqlite3_bind_int(ins, 3, y);
    if (sqlite3_bind_blob(ins, 4, buf, sz, SQLITE_TRANSIENT) != SQLITE_OK) {
        fail("Bind blob failed", db);
    }
    if (sqlite3_step(ins) != SQLITE_DONE) {
        fail("Insert failed", db);
    }
    sqlite3_finalize(ins);
    sqlite3_close(db);

    std::cout << "Created test.routingdb with 1 tile (z=" << z << " x=" << x << " y=" << y << ")\n";
    return 0;
}
