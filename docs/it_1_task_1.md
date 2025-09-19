# Спецификация задачи 1: Парсер OSM (PBF) → тайлы Land Graph → SQLite + FlatBuffers

## 1. Цель

Реализовать инструмент (`converter`), который из OSM PBF-файла (страна/регион) создаёт один контейнер `.routingdb` с тайлами land-графа, пригодный для оффлайн-маршрутизации.

## 2. Исходные данные

- **Вход**: `input.osm.pbf` — PBF-файл отдельной страны (например `russia-latest.osm.pbf`) или региона (например Архангельская область).
- **Параметры**: конвертер сам нарезает входные данные на тайлы 4×4 км.
- **Выход**: `output.routingdb` — SQLite контейнер с таблицей тайлов и метаданными.

## 3. Структура контейнера SQLite

### 3.1 Таблица `land_tiles`

```sql
CREATE TABLE land_tiles (
  z INTEGER NOT NULL,
  x INTEGER NOT NULL,
  y INTEGER NOT NULL,
  lat_min REAL NOT NULL,
  lon_min REAL NOT NULL,
  lat_max REAL NOT NULL,
  lon_max REAL NOT NULL,
  version INTEGER NOT NULL,
  checksum TEXT NOT NULL,
  profile_mask INTEGER NOT NULL,
  data BLOB NOT NULL
);
CREATE UNIQUE INDEX idx_land_tiles_zxy ON land_tiles(z,x,y);
```

- z,x,y — классический идентификатор тайла.
- bbox — координаты тайла.
- version — версия схемы.
- checksum — SHA-256 BLOB данных для контроля.
- profile_mask — битовая маска профилей (Car/Foot/Boat).
- data — FlatBuffers blob графа.

### 3.2 Таблица metadata

```sql
CREATE TABLE metadata (
  key TEXT PRIMARY KEY,
  value TEXT
);
```

- Хранит: версию схемы, дату OSM extract, bbox всего пакета, регион.

## 4. Формат FlatBuffers (BLOB)

### 4.1 Черновая схема

```fbs
namespace Routing;

enum RoadClass : byte {
  MOTORWAY = 0,
  PRIMARY = 1,
  SECONDARY = 2,
  RESIDENTIAL = 3,
  FOOTWAY = 4,
  PATH = 5,
  STEPS = 6
}

table Node {
  id: uint;           // id внутри тайла
  lat_q: int;         // квантованные lat * 1e6
  lon_q: int;         // квантованные lon * 1e6
  first_edge: uint;   // смещение первого ребра
  edge_count: ushort; // количество рёбер
}

table Edge {
  from_node: uint;
  to_node: uint;
  length_m: float;
  speed_mps: float;       // одна скорость по умолчанию (Car)
  foot_speed_mps: float;  // отдельная константа для Foot
  oneway: bool;
  road_class: RoadClass;
  access_mask: ushort;    // биты: 1=car, 2=foot, 4=bike
  shape_start: uint;
  shape_count: ushort;
  encoded_polyline: string; // закодированный polyline
}

table ShapePoint {
  lat_q: int;
  lon_q: int;
}

table LandTile {
  z: ushort;
  x: uint;
  y: uint;
  nodes: [Node];
  edges: [Edge];
  shapes: [ShapePoint];
  version: uint;
  checksum: string;
  profile_mask: uint;
}

root_type LandTile;
```

- lat_q/lon_q = квантованные int32 (lat/lon × 1e6).
- Храним и массив ShapePoint, и encoded polyline (оба варианта).
- У каждого Edge — bitmask доступа.

## 5. Конвертер

### 5.1 CLI

```lua
converter input.osm.pbf output.routingdb
```

### 5.2 Алгоритм работы

1. Считать PBF (libosmium/OSM2) — выделить только нужные объекты:
   - автомобильные и пешеходные пути (highway=* включая motor/residential/footway/path/steps);
   - фильтрация по регионам внутри страны (bbox или poly).
2. Разбить на тайлы 4×4 км по WebMercator или WGS84.
3. Для каждого тайла:
   - собрать массив узлов Node;
   - собрать массив рёбер Edge (с геометрией);
   - рассчитать квантованные координаты;
   - рассчитать скорости: одна для Car, отдельная константа для Foot.
4. Сериализовать LandTile в FlatBuffers.
5. Посчитать SHA-256 checksum, version, profile_mask.
6. Записать строку в land_tiles.
7. Заполнить metadata (версия схемы, дата, bbox, регион).
8. Сформировать лог:
   - количество узлов, рёбер, тайлов;
   - ошибки/предупреждения.

### 5.3 Логи

- По завершении вывести статистику (узлы/рёбра/тайлы).
- По каждому тайлу — warn, если пустой или слишком большой.

### 5.4 Тестовые данные

- Прогнать на регионе «Россия, Архангельская область».
- Проверить:
  - кол-во тайлов,
  - корректность bbox,
  - выборка по z,x,y даёт FlatBuffers, который десериализуется.

### 6. Ожидаемый результат

- На выходе: один файл archangelsk.routingdb.
- Внутри: таблица land_tiles с FlatBuffers, таблица metadata.
- Конвертер CLI выдаёт лог и успешно открывается в ядре маршрутизатора.

---

## 7. Что сделано в рамках задачи (v1)

- Реализован CLI-конвертер (`converter`, C++20, CMake), который:
  - читает OSM PBF через заголовки libosmium/protozero;
  - режет данные на тайлы по схеме WebMercator `z/x/y` (по умолчанию `z=14`, ~4×4 км на экваторе);
  - формирует упрощённый Land Graph (узлы/рёбра) для `highway=*` из спецификации (Car/Foot);
  - сериализует каждый тайл в FlatBuffers (схема `land_tile.fbs`);
  - сохраняет тайлы в SQLite (`land_tiles`) и пишет `metadata`;
  - считает SHA-256 checksum (macOS: CommonCrypto);
  - поддерживает флаг `--z <zoom>` (дефолт 14).
- Поведение по умолчанию: если выходной `.routingdb` существует — удаляется перед записью (детерминированная полная сборка v1).
- Проверка на Liechtenstein PBF — успешно, ~93 тайла.

## 8. Схема работы конвертера (поток данных)

1) CLI парсит аргументы: `converter [--z ZOOM] input.osm.pbf output.routingdb`.
2) Готовит SQLite: пересоздаёт файл, создаёт таблицы `land_tiles`, `metadata`.
3) Парсинг PBF:
   - 1-й проход: сбор всех узлов (id → lat/lon).
   - 2-й проход: выбор `way` с `highway=*`, построение сегментов, определение класса дороги/oneway/access.
   - Привязка сегментов к тайлам по центру сегмента → группировка по `z/x/y`.
4) Для каждого тайла:
   - де-дуп узлов, локальная нумерация; расчёт длины (haversine), скоростей (по классу/профилю);
   - сериализация в FlatBuffers BLOB (`LandTile`);
   - расчёт checksum; запись строки в `land_tiles`.
5) Заполнение `metadata` (версия схемы, путь к источнику и др.).

## 9. Файлы и ответственность

- `converter/src/main.cpp` — CLI-оркестрация:
  - разбор аргументов (`--z`), подготовка выходного файла;
  - вызов `PbfReader::readAndTile()`;
  - сериализация тайлов (`buildLandTileBlob`) и запись в SQLite (`insertLandTile`);
  - лог: Parsed/Written tiles.

- `converter/src/pbf_reader.h, .cpp` — парсинг OSM PBF и нарезка на тайлы:
  - `PbfReader(input_path, zoom)` — конструктор с масштабом;
  - `readAndTile()` — возвращает `unordered_map<key, TileData>`:
    - первый проход по PBF: сбор `Node` в индекс;
    - второй проход: фильтр `highway=*`, построение сегментов, классификация дороги, вычисление тайлового ключа `tileKeyFor(lat, lon, zoom)`;
    - формирование `TileData` (узлы, рёбра, bbox).

- `converter/src/tiler.h` — утилиты тайлинга WebMercator:
  - `tileKeyFor(lat, lon, z)` — вычисление `TileKey{z,x,y}`;
  - `tileBounds(TileKey)` — географический `BBox` тайла.

- `converter/src/serializer.h, .cpp` — сериализация тайла в FlatBuffers:
  - `buildLandTileBlob(const TileData&, uint32_t version, uint32_t profile_mask)` —
    - де-дуп узлов и локальные id;
    - конкатенация форм ребра (shape_start/shape_count);
    - расчёт длины (haversine), скоростей (car по классу, foot ~1.4 м/с);
    - сборка `LandTile` и возврат BLOB.

- `converter/src/sqlite_writer.h, .cpp` — работа с SQLite:
  - `createSchemaIfNeeded()` — создаёт `land_tiles`, индекс `idx_land_tiles_zxy`, `metadata`;
  - `writeMetadata(key, value)` — UPSERT метаданных;
  - `insertLandTile(z,x,y,bbox,version,checksum,profile_mask,blob,sz)` — вставка тайла.

- `converter/src/land_tile.fbs` — схема FlatBuffers:
  - `LandTile { z,x,y, nodes[], edges[], shapes[], version, checksum, profile_mask }`;
  - `Node { id, lat_q, lon_q, first_edge, edge_count }`;
  - `Edge { from_node, to_node, length_m, speed_mps, foot_speed_mps, oneway, road_class, access_mask, shape_start, shape_count, encoded_polyline }`;
  - `ShapePoint { lat_q, lon_q }`.

- `converter/CMakeLists.txt` — сборка:
  - генерация FlatBuffers заголовка (`flatc`), подключение SQLite;
  - подключение заголовков `libosmium/protozero` (Homebrew: `/opt/homebrew/include`), линковка `expat`, `z`, `bz2`, `lz4`, `zstd` при наличии.

## 10. Замечания по использованию

- Запуск:
  - `converter --z 14 input.osm.pbf output.routingdb`
  - по умолчанию выходной файл пересоздаётся; повторный запуск по тому же пути создаёт чистую БД.
- Проверка результата:
  - `sqlite3 output.routingdb "SELECT COUNT(*) FROM land_tiles;"` → `> 0`;
  - `SELECT MIN(z),MAX(z) FROM land_tiles;` → `14,14` (если не меняли `--z`);
  - `SELECT LENGTH(data) FROM land_tiles LIMIT 1;` → `> 0`.

## 11. Дальнейшие улучшения

- Опция `--append` с UPSERT по `z/x/y` для частичных обновлений/дозаливки.
- Расширение набора дорог (при необходимости), speed-модели и доступности.
- Валидаторы: выборочная десериализация FlatBuffers и property-тесты.

## 12. Зависимости и установка

- Сборочные инструменты:
  - CMake ≥ 3.20
  - Компилятор C++ (AppleClang/Xcode CLT на macOS)

- Библиотеки (build-time):
  - FlatBuffers (+ утилита `flatc` для генерации заголовков)
  - SQLite3 (на macOS доступен из SDK; отдельная установка не обязательна)
  - libosmium (headers-only)
  - protozero (headers-only)
  - expat (libexpat)
  - zlib, bzip2, lz4, zstd (поддержка сжатий при чтении данных)

- macOS (Homebrew):

```bash
brew install cmake flatbuffers libosmium protozero expat zstd lz4 bzip2
# (sqlite3 и pkg-config опциональны; для libosmium pkg-config не требуется)
```

- Пути заголовков:
  - По умолчанию CMake ищет `libosmium/protozero` в `/opt/homebrew/include` и `/usr/local/include`.
  - При нестандартном расположении укажите явно:

```bash
cmake -S . -B build \
  -DOSMIUM_INCLUDE_DIR=/custom/include \
  -DPROTOZERO_INCLUDE_DIR=/custom/include
```

- Примечания:
  - На macOS checksum считается через CommonCrypto. На Linux/BSD можно подключить OpenSSL для SHA-256 (опционально).
  - Для проверки результата удобно иметь `sqlite3` CLI.

- Запуск и проверка

```bash
cd /Users/borovinsky.in/dev/LoxxCore
cmake -S . -B build
cmake --build build -j 4
```

```bash
curl -L -o /tmp/liechtenstein.osm.pbf https://download.geofabrik.de/europe/liechtenstein-latest.osm.pbf
./build/converter/converter --z 14 /tmp/liechtenstein.osm.pbf /Users/borovinsky.in/dev/LoxxCore/build/test.routingdb
```

Ожидания:
Parsed tiles: N (> 0)
Written tiles: N (совпадает)
В SQLite таблица land_tiles непуста:

```bash
sqlite3 ./build/test.routingdb "SELECT COUNT(*) FROM land_tiles;"
```
