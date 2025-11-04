# ✅ Migration Checklist — Quick Reference

Краткий чеклист для миграции на multi-repo.

## 📋 Pre-migration

- [ ] Backup текущего кода: `cp -r bindings/ios ~/Desktop/loxx-ios-backup`
- [ ] Создать ветку: `git checkout -b migrate-to-multirepo`

## 🔧 loxx-core setup

- [ ] Переименовать GitHub repo: `loxx_core` → `loxx-core`
- [ ] Обновить remote: `git remote set-url origin https://github.com/ilisun/loxx-core.git`
- [ ] Удалить bindings: `git rm -r bindings/`
- [ ] Добавить build script: `scripts/build-ios-framework.sh` ✅
- [ ] Добавить GitHub Actions: `.github/workflows/release-ios.yml` ✅
- [ ] Обновить README
- [ ] Push: `git push origin migrate-to-multirepo`
- [ ] Merge PR → `main`

## 📦 loxx-router-ios создание

- [ ] Создать новый repo на GitHub: `loxx-router-ios`
- [ ] Клонировать: `git clone https://github.com/ilisun/loxx-router-ios`
- [ ] Скопировать код из backup
- [ ] Создать Package.swift с `.binaryTarget()`
- [ ] Создать .gitignore
- [ ] Commit & push

## 🚀 Первый релиз

- [ ] В loxx-core: `git tag v1.0.0 && git push origin v1.0.0`
- [ ] Дождаться GitHub Actions (5-10 мин)
- [ ] Проверить Release создан
- [ ] Скопировать checksum из logs
- [ ] Обновить Package.swift в loxx-router-ios
- [ ] В loxx-router-ios: `git tag v1.0.0 && git push origin v1.0.0`

## 🧪 Тестирование

- [ ] Создать тестовый Xcode проект
- [ ] Добавить пакет: `https://github.com/ilisun/loxx-router-ios`
- [ ] Написать простой тест:
```swift
import LoxxRouter
let router = try LoxxRouter.bundled()
```
- [ ] Собрать проект (Cmd+B)
- [ ] ✅ Успех!

## 📚 Финализация

- [ ] Обновить документацию в обоих репозиториях
- [ ] Добавить cross-references в README
- [ ] Создать GitHub Releases с Release Notes
- [ ] Архивировать старую структуру: `git tag archive/monorepo-before-split`

---

## 🆘 Quick Fixes

**Checksum mismatch:**
```bash
swift package compute-checksum LoxxCore.xcframework.zip
```

**XCFramework не скачивается:**
- Release должен быть публичным (не draft)
- Проверить URL в Package.swift

**Build fails:**
```bash
brew install cmake sqlite3
./scripts/build-ios-framework.sh
```

---

Полная инструкция: [MIGRATION_TO_MULTIREPO.md](MIGRATION_TO_MULTIREPO.md)

