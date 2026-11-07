# Sonasphere

Sonasphere — разрабатываемая среда для live coding музыки в духе Sonic Pi, Strudel и TidalCycles.
Проект написан на C++23 и рассчитан на Linux; интерфейс — CLI.

Музыкальный язык **sona** отвечает за композицию, паттерны и темп, а аудиодвижок **bitvm** — за
DSP-графы и точное исполнение команд. Их связывает отдельный протокол; язык не зависит от движка,
а движок не знает о музыкальном синтаксисе.

## Текущее состояние

- [x] Протокол bitvm v0.1: бинарный кодек, текстовая форма, проверка команд и графов, каталог юнитов,
  транспорт Unix `SOCK_SEQPACKET`;
- [x] Ядро движка: DSP-графы, дерево узлов, аудио- и контрольные шины, расписание с точностью до сэмпла
  и очереди между control- и audio-потоками;
- [x] Offline-рендер текстовых и бинарных партитур в память или float32 WAV, утилита `bitvm-render`;
- [ ] Демон `bitvmd`, сокет-сервер, вывод через ALSA и утилита `bitvmctl`;
- [ ] Расширенный набор DSP-юнитов: осцилляторы, шум, фильтры, delay, reverb и воспроизведение сэмплов;
- [ ] Дизайн и реализация языка sona, паттерны, темп, REPL и стандартная библиотека;
- [ ] Исполнение `.sona`, слежение за изменениями файлов, сохранение и загрузка сессий, рендер композиций.

`sona` и демон `bitvmd` пока остаются каркасами. Список обновляется после завершения крупных
возможностей. Подробная дорожная карта — в [архитектуре, §9](docs/architecture.md#9-дорожная-карта).

## Сборка

Нужны Linux, компилятор и стандартная библиотека с поддержкой используемых возможностей C++23
(включая `std::expected`), Meson **1.3.0+**, Ninja, pkg-config и pthreads. Текущая локальная сборка
использует GCC 16.

Дополнительные зависимости управляются опциями Meson:

| Зависимость | Опция | По умолчанию | Назначение |
|-------------|-------|--------------|------------|
| libsndfile | `sndfile` | `enabled` | Загрузка аудиофайлов через `buf.load` |
| GoogleTest | `tests` | `auto` | Тесты, если библиотека найдена |
| alsa-lib | `alsa` | `auto` | Зависимость для будущего ALSA-бэкенда |

Для сборки и обязательного включения тестов:

```bash
meson setup build -Dtests=enabled
meson compile -C build
meson test -C build --print-errorlogs
```

Без GoogleTest, libsndfile и ALSA можно собрать движок и рендерер так:

```bash
meson setup build-minimal -Dtests=disabled -Dsndfile=disabled -Dalsa=disabled
meson compile -C build-minimal
```

Запись WAV не требует libsndfile; при `-Dsndfile=disabled` недоступна загрузка аудиофайлов.

## Попробовать offline-рендер

Из корня репозитория после сборки:

```bash
build/bitvm/bitvm-render --duration 2 examples/offline/ping.bvs build/ping.wav
```

Результат — двухсекундный стереофайл `build/ping.wav`, 48 кГц, float32: три короткие ноты
440, 660 и 880 Гц. Пример состоит из [партитуры `ping.bvs`](examples/offline/ping.bvs) и
[DSP-графа `ping.bvg`](examples/offline/ping.bvg). Это текстовые форматы bitvm; рендерер читает
также бинарные партитуры `.bvsb`. Пути к графам и аудиофайлам разрешаются относительно партитуры.

Флаги `--sample-rate` и `--channels` меняют формат выхода. Без `--duration` рендер заканчивается
через секунду после последнего бандла.

## Тесты

Отдельные наборы проверяют протокол и движок:

```bash
meson test -C build --suite shared --print-errorlogs
meson test -C build --suite bitvm --print-errorlogs
```

Набор `sona` пока пуст. Для AddressSanitizer и UndefinedBehaviorSanitizer:

```bash
meson setup build-asan -Dtests=enabled -Db_sanitize=address,undefined
meson compile -C build-asan
meson test -C build-asan --print-errorlogs
```

Проверки декодера и текстового парсера с libFuzzer требуют Clang:

```bash
CXX=clang++ meson setup build-fuzz -Dfuzz=true -Db_sanitize=address,undefined
meson compile -C build-fuzz
meson test -C build-fuzz --suite fuzz --print-errorlogs
```

[GitHub Actions workflow `tests`](.github/workflows/tests.yml) собирает проект и запускает тесты
на Ubuntu 24.04 с GCC 14 при push в `main`. После появления workflow в основной ветке его можно
запустить вручную: **Actions → tests → Run workflow**.

## Устройство репозитория

| Каталог | Содержимое |
|---------|------------|
| [`shared/`](shared/README.md) | Общий протокол bitvm: команды, кодеки, GraphDef, каталог и транспорт |
| [`bitvm/`](bitvm/README.md) | Аудиодвижок, offline-рендерер и каркас демона `bitvmd` |
| [`sona/`](sona/README.md) | Каркас языка и CLI; будущие паттерны, темп, планировщик и REPL |
| [`stdlib/`](stdlib/README.md) | Место для будущей стандартной библиотеки sona |
| [`examples/`](examples/README.md) | Рабочий пример offline-партитуры; примеры `.sona` появятся вместе с языком |
| [`docs/`](docs/) | Архитектура, протокол и результаты этапов |

В audio-потоке запрещены аллокации, освобождение памяти, мьютексы, ввод-вывод и исключения.
Все ресурсы и команды готовятся на control-стороне. Зависимость sona ограничена протоколом
`bitvm_proto_dep`; код аудиодвижка ей недоступен.

## Документация и разработка

- [Архитектура, принятые решения и дорожная карта](docs/architecture.md)
- [Протокол bitvm v0.1 и форматы `.bvg`, `.bvs`, `.bvsb`](docs/protocol.md)
- [Правила работы с репозиторием](AGENTS.md)

Синтаксис sona проектируется отдельно. Новые возможности реализуются по этапам
из дорожной карты; архитектурные решения и изменения протокола отражаются в документации.

## Лицензия

[GNU General Public License, version 2](LICENSE).
