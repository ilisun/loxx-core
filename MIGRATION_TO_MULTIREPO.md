# 🔄 Миграция на Multi-repo структуру

Пошаговое руководство по разделению monorepo на отдельные репозитории.

## 📋 Итоговая структура

```
loxx-core/              → C++ ядро + автосборка XCFramework
  ├── core/             → Routing engine
  ├── converter/        → PBF → routingdb
  ├── scripts/          → Build scripts
  └── .github/workflows → CI/CD

loxx-router-ios/        → Swift SDK (новый репозиторий)
  ├── Package.swift     → Binary dependency на loxx-core
  ├── Sources/          → Pure Swift API
  ├── Tests/            → Unit tests
  └── Examples/         → Demo app
```

---

## Этап 1: Подготовка loxx-core

### 1.1. Переименовать репозиторий на GitHub

1. Перейти в https://github.com/ilisun/loxx_core
2. **Settings → General → Repository name**
3. Изменить: `loxx_core` → `loxx-core`
4. **Rename**

### 1.2. Обновить локальный remote

```bash
cd /Users/borovinsky.in/dev/my/loxx/LoxxCore

# Обновить URL
git remote set-url origin https://github.com/ilisun/loxx-core.git

# Проверить
git remote -v
```

### 1.3. Создать ветку для миграции

```bash
git checkout -b migrate-to-multirepo
```

### 1.4. Удалить iOS bindings (переедут в отдельный repo)

```bash
# Сохранить резервную копию
mkdir -p ~/Desktop/loxx-migration-backup
cp -r bindings/ios ~/Desktop/loxx-migration-backup/

# Удалить из loxx-core
git rm -r bindings/
git commit -m "Remove iOS bindings - migrating to loxx-router-ios"
```

### 1.5. Добавить scripts и CI/CD

```bash
# Скрипты уже созданы в scripts/build-ios-framework.sh
# GitHub Actions уже в .github/workflows/release-ios.yml

git add scripts/build-ios-framework.sh
git add .github/workflows/release-ios.yml
git commit -m "Add XCFramework build automation"
```

### 1.6. Обновить README.md

```bash
cat > README.md << 'EOF'
# LoxxCore — C++ Routing Engine

Кроссплатформенное ядро маршрутизации на основе OpenStreetMap.

## 🎯 Использование

### iOS
См. [loxx-router-ios](https://github.com/ilisun/loxx-router-ios) для Swift API

### Android (Coming soon)
См. [loxx-router-android](https://github.com/ilisun/loxx-router-android)

### CLI Converter
```bash
cmake -S . -B build
cmake --build build
./build/converter/converter --z 14 input.osm.pbf output.routingdb
\`\`\`

## 📦 Releases

Каждый релиз включает:
- `LoxxCore.xcframework.zip` — iOS binary
- `converter` — CLI tool (macOS/Linux)

## 🔧 Разработка

См. [docs/](docs/) для документации C++ API.
EOF

git add README.md
git commit -m "Update README for multi-repo structure"
```

### 1.7. Push и создать PR

```bash
git push origin migrate-to-multirepo

# Создать Pull Request на GitHub
# После review: merge в main
```

---

## Этап 2: Создать loxx-router-ios репозиторий

### 2.1. Создать новый репозиторий на GitHub

1. Перейти https://github.com/new
2. **Repository name:** `loxx-router-ios`
3. **Description:** Swift SDK for LoxxCore routing engine
4. **Public**
5. ✅ Add README
6. **Create repository**

### 2.2. Клонировать и настроить

```bash
cd ~/Desktop/loxx-migration-backup
git clone https://github.com/ilisun/loxx-router-ios.git
cd loxx-router-ios

# Скопировать iOS bindings
cp -r ../ios/Sources ./
cp -r ../ios/Tests ./
cp -r ../ios/Examples ./
cp ../ios/README.md ./
cp ../ios/QUICKSTART.md ./
cp ../ios/INTEGRATION_GUIDE.md ./
```

### 2.3. Создать Package.swift с binary target

```bash
cat > Package.swift << 'EOF'
// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "LoxxRouter",
    platforms: [
        .iOS(.v13),
        .macCatalyst(.v13)
    ],
    products: [
        .library(
            name: "LoxxRouter",
            targets: ["LoxxRouter"]
        ),
    ],
    targets: [
        // ═══════════════════════════════════════════════════
        // Public Swift API
        // ═══════════════════════════════════════════════════
        .target(
            name: "LoxxRouter",
            dependencies: ["LoxxRouterCore"],
            path: "Sources/LoxxRouter",
            swiftSettings: [
                .enableExperimentalFeature("StrictConcurrency")
            ]
        ),
        
        // ═══════════════════════════════════════════════════
        // Private Objective-C++ Bridge
        // ═══════════════════════════════════════════════════
        .target(
            name: "LoxxRouterBridge",
            dependencies: ["LoxxRouterCore"],
            path: "Sources/LoxxRouterBridge",
            publicHeadersPath: "include"
        ),
        
        // ═══════════════════════════════════════════════════
        // C++ Core (Binary Target from loxx-core releases)
        // ═══════════════════════════════════════════════════
        // TODO: Замените на реальный URL после первого релиза loxx-core
        .binaryTarget(
            name: "LoxxRouterCore",
            url: "https://github.com/ilisun/loxx-core/releases/download/v1.0.0/LoxxCore.xcframework.zip",
            checksum: "PLACEHOLDER_CHECKSUM"
        ),
        
        // ═══════════════════════════════════════════════════
        // Tests
        // ═══════════════════════════════════════════════════
        .testTarget(
            name: "LoxxRouterTests",
            dependencies: ["LoxxRouter"],
            path: "Tests/LoxxRouterTests",
            resources: [.copy("Resources")]
        ),
    ]
)
EOF
```

### 2.4. Обновить Swift код

Нужно убрать прямые импорты C++ заголовков, использовать только через bridge:

```bash
# В Sources/LoxxRouter/LoxxRouter.swift
# Заменить:
# import LoxxRouterCore  // C++ headers
# На:
# import LoxxRouterBridge  // Objective-C++ bridge

# Это потребует небольшого рефакторинга
```

### 2.5. Создать .gitignore

```bash
cat > .gitignore << 'EOF'
# Swift Package Manager
.build/
.swiftpm/
Package.resolved

# Xcode
*.xcodeproj
*.xcworkspace
xcuserdata/
DerivedData/

# macOS
.DS_Store
EOF
```

### 2.6. Коммит и push

```bash
git add .
git commit -m "Initial iOS SDK structure"
git push origin main
```

---

## Этап 3: Первый релиз loxx-core

### 3.1. В loxx-core создать релиз

```bash
cd /Users/borovinsky.in/dev/my/loxx/LoxxCore

# Убедиться что в main ветке
git checkout main
git pull

# Создать тег
git tag -a v1.0.0 -m "Release v1.0.0: C++ core with iOS XCFramework"
git push origin v1.0.0
```

### 3.2. GitHub Actions автоматически:

1. Соберет XCFramework
2. Создаст Release v1.0.0
3. Загрузит `LoxxCore.xcframework.zip`
4. Выведет checksum в комментарии

### 3.3. Получить checksum

```bash
# Способ 1: Из GitHub Actions logs
# Перейти: Actions → Build iOS XCFramework → View logs
# Найти строку с checksum

# Способ 2: Скачать и вычислить локально
curl -L -o LoxxCore.xcframework.zip \
  https://github.com/ilisun/loxx-core/releases/download/v1.0.0/LoxxCore.xcframework.zip

swift package compute-checksum LoxxCore.xcframework.zip
# Вывод: abc123...
```

---

## Этап 4: Обновить loxx-router-ios с реальным checksum

### 4.1. Обновить Package.swift

```bash
cd ~/path/to/loxx-router-ios

# Заменить в Package.swift:
# url: "https://github.com/ilisun/loxx-core/releases/download/v1.0.0/LoxxCore.xcframework.zip",
# checksum: "РЕАЛЬНЫЙ_CHECKSUM_ИЗ_STEP_3.3"

git add Package.swift
git commit -m "Update LoxxCore binary target with real checksum"
git push origin main
```

### 4.2. Протестировать локально

```bash
# Открыть Package.swift в Xcode
open Package.swift

# Или собрать через CLI
swift build

# Xcode скачает XCFramework и попробует собрать
```

### 4.3. Создать первый релиз iOS SDK

```bash
git tag -a v1.0.0 -m "Release v1.0.0: Swift SDK for iOS"
git push origin v1.0.0

# Создать GitHub Release вручную с Release Notes
```

---

## Этап 5: Тестирование интеграции

### 5.1. Создать тестовый iOS проект

```bash
mkdir ~/Desktop/LoxxRouterTest
cd ~/Desktop/LoxxRouterTest

# Создать новый Xcode проект
# File → New → Project → iOS App
```

### 5.2. Добавить пакет

1. **File → Add Package Dependencies**
2. URL: `https://github.com/ilisun/loxx-router-ios`
3. Version: `1.0.0`
4. **Add Package**

### 5.3. Проверить что работает

```swift
import SwiftUI
import LoxxRouter

@main
struct TestApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
        }
    }
}

struct ContentView: View {
    var body: some View {
        Button("Test Router") {
            do {
                // Должно скомпилироваться!
                let router = try LoxxRouter.bundled()
                print("✅ Router initialized successfully!")
            } catch {
                print("❌ Error: \(error)")
            }
        }
    }
}
```

### 5.4. Собрать проект

```bash
# В Xcode: Cmd+B
# Или через CLI:
xcodebuild -scheme LoxxRouterTest -destination 'platform=iOS Simulator,name=iPhone 15'
```

---

## Этап 6: Обновить документацию

### 6.1. В loxx-router-ios обновить INTEGRATION_GUIDE.md

```markdown
## Установка

```swift
// В Xcode:
// File → Add Package Dependencies
// URL: https://github.com/ilisun/loxx-router-ios

dependencies: [
    .package(url: "https://github.com/ilisun/loxx-router-ios", from: "1.0.0")
]
\`\`\`
```

### 6.2. Добавить badge в README

```markdown
[![Platform](https://img.shields.io/badge/Platform-iOS%2013.0+-lightgrey.svg)]()
[![Swift](https://img.shields.io/badge/Swift-5.9-orange.svg)]()
[![SPM](https://img.shields.io/badge/SPM-compatible-brightgreen.svg)]()
```

---

## Этап 7: Cleanup и финализация

### 7.1. Обновить главный README loxx-core

```bash
cd /Users/borovinsky.in/dev/my/loxx/LoxxCore

cat >> README.md << 'EOF'

## 📱 Mobile SDKs

- **iOS:** [loxx-router-ios](https://github.com/ilisun/loxx-router-ios)
- **Android:** Coming soon

## 🔗 Related Repositories

- [loxx-router-ios](https://github.com/ilisun/loxx-router-ios) — Swift SDK
- [loxx-router-android](https://github.com/ilisun/loxx-router-android) — Kotlin SDK (planned)
EOF

git add README.md
git commit -m "Add links to mobile SDKs"
git push origin main
```

### 7.2. Архивировать старую структуру

```bash
# В корне проекта создать архив
cd /Users/borovinsky.in/dev/my/loxx/LoxxCore
git tag -a archive/monorepo-before-split -m "Archive: monorepo structure before multi-repo migration"
git push origin archive/monorepo-before-split
```

---

## ✅ Проверка что всё работает

### Чеклист

- [ ] loxx-core на GitHub переименован в loxx-core
- [ ] loxx-core имеет GitHub Actions для сборки XCFramework
- [ ] loxx-core v1.0.0 релиз создан с LoxxCore.xcframework.zip
- [ ] loxx-router-ios репозиторий создан
- [ ] loxx-router-ios Package.swift использует правильный checksum
- [ ] loxx-router-ios v1.0.0 релиз создан
- [ ] Тестовый iOS проект успешно компилируется
- [ ] Документация обновлена

---

## 🚀 Процесс обновления в будущем

### Обновление C++ ядра:

```bash
cd loxx-core
# Внести изменения
git commit -m "Fix: memory leak in tile cache"
git tag v1.0.1
git push origin v1.0.1
# GitHub Actions → build → release
```

### Обновление iOS SDK:

```bash
cd loxx-router-ios
# Обновить Package.swift с новым checksum
git commit -m "Update LoxxCore to v1.0.1"
git tag v1.0.1
git push origin v1.0.1
```

---

## 🆘 Troubleshooting

### Ошибка: "Binary target checksum mismatch"

```bash
# Пересчитать checksum
swift package compute-checksum LoxxCore.xcframework.zip
# Обновить в Package.swift
```

### Ошибка: "Failed to download binary target"

- Проверить URL в Package.swift
- Убедиться что Release публичный (не draft)
- Проверить что файл загружен на GitHub

### XCFramework не собирается

```bash
# Проверить зависимости
brew install cmake sqlite3

# Запустить скрипт вручную
./scripts/build-ios-framework.sh
```

---

## 📚 Дополнительные ресурсы

- [Creating an XCFramework](https://developer.apple.com/documentation/xcode/creating-a-multi-platform-binary-framework-bundle)
- [Distributing Binary Frameworks as Swift Packages](https://developer.apple.com/documentation/xcode/distributing-binary-frameworks-as-swift-packages)
- [GitHub Actions for iOS](https://docs.github.com/en/actions/deployment/deploying-xcode-applications)

