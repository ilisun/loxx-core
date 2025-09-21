# Спецификация задачи 2: Реализация C++-ядра (структуры графа, чтение тайлов, bi-A*)

## 1. Цель

Реализовать ядро маршрутизации на C++17/20, которое:

- открывает один контейнер `.routingdb` (SQLite с FlatBuffers-тайлами),
- лениво подгружает нужные тайлы,
- строит маршруты профилей Car/Foot с помощью bi-A*,
- возвращает polyline, расстояние, ETA и список рёбер,
- снапит старт/финиш к ближайшему ребру (edge).

## 2. Исходные данные

- Контейнер `.routingdb`, созданный конвертером (см. задачу 1).
- Внутри: таблица `land_tiles` с FlatBuffers (LandTile) + таблица `metadata`.

## 3. Архитектура ядра

### 3.1 Класс Router

```cpp
enum class Profile { Car, Foot };

enum class RouteStatus {
    OK,
    NO_ROUTE,       // маршрут не найден
    NO_TILE,        // точка вне данных
    DATA_ERROR,     // повреждён контейнер/тайл
    INTERNAL_ERROR  // непредвиденная ошибка
};

struct Coord {
    double lat;
    double lon;
};

struct RouteResult {
    RouteStatus status;
    std::vector<Coord> polyline;       // маршрут
    double distance_m = 0.0;           // расстояние
    double duration_s = 0.0;           // ETA
    std::vector<uint64_t> edge_ids;    // список рёбер маршрута
    std::string error_message;         // описание ошибки
};

class Router {
public:
    explicit Router(const std::string& db_path); // открыть SQLite-контейнер
    RouteResult route(Profile profile,
                      const std::vector<Coord>& waypoints);
};
```

- Router создаётся один раз на нужный .routingdb.
- Метод route() принимает профиль и список точек (start, waypoints, end).

### 3.2 Работа с контейнером

- SQLite открывается один раз в конструкторе.
- Тайлы читаются лениво:
  - SELECT data FROM land_tiles WHERE z=? AND x=? AND y=?;
  - FlatBuffers десериализация без копирования (flatbuffers::GetRoot<LandTile>).
- LRU-кэш на N тайлов (например 128) для повторного использования.

### 3.3 Структура данных в памяти

- Работаем прямо по FlatBuffers (zero-copy):
  - узлы и рёбра читаются из blob-а;
  - индексы и координаты берутся по указателям FlatBuffers;
  - нет аллокаций и копий.

### 3.4 Снап старт/финиш

- Находим ближайшее ребро (edge) к заданной координате:
  - ищем кандидатов в текущем и соседних тайлах;
  - проектируем точку на геометрию ребра (Haversine);
  - создаём “виртуальный” узел/ребро для старта/финиша.
- Поиск маршрута начинается/заканчивается на этих виртуальных рёбрах.
- Это даёт точное попадание в координату пользователя.

## 4. Алгоритм bi-A*

### 4.1 Основы

- Двунаправленный A* (поиск от старта и от финиша).
- Две приоритетные очереди (std::priority_queue).
- Используем ID узлов/рёбер как ключи.
- Критерий завершения: фронты встретились.

### 4.2 Эвристика

- Haversine (great-circle) расстояние между текущим узлом и финишем.
- cost = расстояние / скорость (м/с).

### 4.3 Стоимость (cost)

- Car: скорость = edge.speed_mps, cost = length / speed.
- Foot: скорость = edge.foot_speed_mps, cost = length / foot_speed.

### 4.4 Выход

- После встречи фронтов строим путь (узлы/рёбра) от старта до финиша.
- Возвращаем:
  - polyline — координаты всего маршрута;
  - distance_m — сумма длин рёбер;
  - duration_s — сумма cost (сек);
  - edge_ids — список рёбер маршрута.

## 5. Ошибки

- Если точка вне данных — RouteStatus::NO_TILE.
- Если нет пути между точками — RouteStatus::NO_ROUTE.
- Если тайл повреждён — RouteStatus::DATA_ERROR
- В error_message — текстовое описание (опционально).

## 6. Поток работы ядра

- Создать Router: Router r("archangelsk.routingdb");.
- Снапить точки к ближайшему ребру (Router делает сам).
- bi-A* от старта к финишу с профилем.
- Вернуть RouteResult.

## 7. Тестирование

- Юнит-тесты:
  - чтение контейнера, десериализация тайла;
  - поиск ближайшего ребра;
  - bi-A* на маленьком графе (ручные узлы/рёбра).

- Интеграционные:
  - построить маршрут Car/Foot по Архангельской области (заранее известный путь);
  - проверить polyline, distance, ETA.

- Отладка:
  - экспорт polyline маршрута в GeoJSON и визуализация в QGIS/geojson.io.

## 8. Ожидаемый результат

- Ядро, собираемое как статическая библиотека (.a/.xcframework/.aar).
- Умеет открывать один .routingdb, строить маршруты Car/Foot.
- Возвращает polyline, расстояние, ETA, список рёбер.
- Снапит координаты к ребру.

---

## 9. Структура проекта (`core/`)

- `core/CMakeLists.txt` — сборка статической библиотеки `routing_core` и примера `route_demo`; генерация FlatBuffers заголовка (`land_tile_generated.h`) в `build/core/generated`.

- `core/include/routing_core/`
  - `router.h` — публичный API ядра:
    - `Profile`, `RouteStatus`, `Coord`, `RouteResult`;
    - `class Router { Router(db_path); RouteResult route(profile, waypoints); }`.
  - `tile_store.h` — доступ к SQLite контейнеру, загрузка BLOB тайла (`load(z,x,y)` → BLOB во владельце `shared_ptr<vector<uint8_t>>`).
  - `tile_view.h` — zero-copy представление тайла поверх FlatBuffers (`nodeCount/edgeCount/nodeLat/nodeLon/edge`).
  - `tiler.h` — утилиты WebMercator (`webTileKeyFor(lat, lon, z)` → `{z,x,y}`).

- `core/src/`
  - `router.cpp` — реализация `Router`:
    - открытие контейнера, загрузка тайла старта;
    - снап к ближайшим узлам тайла;
    - упрощённый A*/Dijkstra внутри одного тайла (cost = length/speed по профилю);
    - формирование `RouteResult` (polyline, distance, duration, статус/ошибка).
  - `tile_store.cpp` — реализация доступа к SQLite (`SELECT data FROM land_tiles ...`), копирование BLOB в `vector<uint8_t>`.

- `core/examples/`
  - `route_demo.cpp` — пример запуска: `route_demo routingdb lat1 lon1 lat2 lon2`, печать результата либо статуса ошибки (`NO_TILE`, `NO_ROUTE`, ...).

- Сгенерированный FlatBuffers заголовок
  - `build/core/generated/land_tile_generated.h` (из схемы `converter/src/land_tile.fbs`).

Примечание: текущая реализация маршрутизирует в пределах одного тайла (`z=14` по умолчанию). Следующий шаг — многотайловый поиск (загрузка 3×3 тайлов, стыковка узлов на границах) и формирование `edge_ids`.
