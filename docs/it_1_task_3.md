# Спецификация задачи 3: Реализация профилей маршрутизации Car / Foot

## 1. Цель

Добавить поддержку профилей маршрутизации «Машина» и «Пешком» в ядро.
Профиль управляет:

- допустимостью рёбер (access_mask);
- скоростями по классам дорог (RoadClass);
- алгоритм берёт скорость и доступность из профиля, а не из тайлов.

## 2. Контекст

В .routingdb хранятся базовые данные рёбер: length_m, road_class, access_mask.
Скорости по профилям должны храниться в коде/конфиге, как в Valhalla.
API роутера уже реализован с bi-A*внутри тайла и мультитайловым A*.

## 3. Изменения API ядра

- Убрать enum Profile {Car,Foot}.
- Добавить тип:

```cpp
struct ProfileSettings {
  uint16_t access_mask;
  std::array<double, (int)Routing::RoadClass::MAX+1> speeds_mps;
};
```

- У роутера сделать:

```cpp
RouteResult route(const ProfileSettings& profile,
                  const std::vector<Coord>& waypoints);
```

Роутер теперь работает напрямую с ProfileSettings.

## 4. Реализация профилей по умолчанию

Добавить core/include/routing_core/profile.h:

```cpp
namespace routing_core {

inline ProfileSettings makeCarProfile();
inline ProfileSettings makeFootProfile();

} // namespace routing_core
```

### CarProfile

- access_mask = 1 (только машины);
- скорости м/с:
  - Motorway ≈30
  - Primary ≈25
  - Secondary ≈20
  - Residential ≈15
  - Footway/Path/Steps ≈1

### FootProfile

- access_mask = 2 (только пешеход);
- скорости м/с:
  - Motorway/Primary/Secondary = 0 (запрещено)
  - Residential/Footway/Path = 1.4
  - Steps = 1.0

## 5. Изменения внутри маршрутизатора

При обходе рёбер A*:

```cpp
auto rc = static_cast<int>(e->road_class());
if (!(profile.access_mask & e->access_mask())) continue; // нет доступа
double speed = profile.speeds_mps[rc];
if (speed <= 0.0) continue;
double w = e->length_m() / speed;
```

Также учитывать oneway: если движение запрещено в обратную сторону, не добавлять обратное ребро.

### Снап к ближайшему ребру

Снап только к рёбрам, которые допустимы профилем (т.е. speed>0 и (profile.access_mask & access_mask)!=0).

## 6. Изменения примера route_demo

- Подключить profile.h.
- По умолчанию использовать makeCarProfile().
- Если среди аргументов есть --foot, использовать makeFootProfile().
- Если среди аргументов есть --dump, включить дамп отладочной информации.

## 7. Требования к тестам

- На искусственном тайле (3 узла, 2 ребра) проверить:
  - CarProfile: маршрут строится, расстояние ≈160 м, duration ≈12 с.
  - FootProfile: маршрут строится, расстояние ≈160 м, duration ≈115 с.

- На реальных данных (Лихтенштейн) проверить, что Car и Foot маршруты различаются.

## 8. Возможные варианты реализации

- В будущем можно грузить профили из JSON (Valhalla-подход).
- Можно добавить дополнительные профили (Bike, Boat), не меняя тайлов.

## 9. Ошибки/исключения

- Если профилю нет доступа ни к одному ребру в зоне поиска, вернуть NO_ROUTE.
- Если tile отсутствует, вернуть NO_TILE.

## 10. План работы

1. Создать profile.h с ProfileSettings и фабричными функциями.
2. Изменить API Router::route → принимать ProfileSettings.
3. Изменить алгоритм A* и снап – использовать ProfileSettings для скорости и доступа.
4. Обновить route_demo (по умолчанию Car, --foot для Foot).
5. Протестировать.

---

## 11. Что сделано в рамках задачи

- Профили маршрутизации реализованы через `ProfileSettings` (access_mask + speeds_mps по `Routing::RoadClass`).
  - Добавлен заголовок `core/include/routing_core/profile.h` с фабриками `makeCarProfile()` и `makeFootProfile()`.
  - Car: access_mask=1; скорости м/с: Motorway=30, Primary=25, Secondary=20, Residential=15, Footway/Path/Steps=1.0.
  - Foot: access_mask=2; скорости м/с: Motorway/Primary/Secondary=0, Residential/Footway/Path=1.4, Steps=1.0.

- Изменён публичный API роутера:
  - `RouteResult route(const ProfileSettings&, const std::vector<Coord>&)` — роутер берёт скорость и доступ к ребру из профиля (а не из тайла).

- Алгоритм поиска и снап:
  - Снап к ближайшему ребру фильтрует кандидатов по профилю: `(profile.access_mask & edge.access_mask)!=0` и `profile.speed[road_class]>0`.
  - Вес ребра `w = length_m / profile.speeds_mps[road_class]`; oneway учитывается при построении направленных рёбер.
  - Внутри тайла — bi‑A*.
  - Мультитайл — объединение графа из прямоугольника тайлов (динамическая рамка по расстоянию), коннекторы по точному `(lat_q,lon_q)`, bi‑A* по глобальному графу.

- Пример `core/examples/route_demo.cpp` обновлён:
  - Флаги: `car|foot`, `--z <zoom>`, `--dump`.
  - По умолчанию используется `makeCarProfile()`.

- Тестирование (ручное):
  - Искусственный тайл (3 узла, 2 ребра): маршрут строится, polyline корректен; различия ETA для Car/Foot соответствуют заданным скоростям.
  - Реальные данные (Лихтенштейн):
    - Короткие и длинные маршруты строятся для Car/Foot; Car быстрее и часто длиннее по расстоянию, Foot — медленнее, избегает магистралей (speed=0).

- Дополнительно:
  - Добавлен LRU‑кэш тайлов в `TileStore` с настраиваемой ёмкостью (`RouterOptions.tileCacheCapacity`).
  - Упаковка `edge_id` стандартизирована в `edge_id.h`: 64 бита `[z:8][x:20][y:20][edgeIdx:16]`; восстановление polyline по `(z,x,y,edgeIdx)`.

### Известные ограничения
- Дальнейшая оптимизация памяти/скорости при очень длинных маршрутах возможна (выбор рамки/эвристики).
- Параметры профилей вынесены в код; загрузка из JSON (Valhalla‑подход) — будущая задача.
