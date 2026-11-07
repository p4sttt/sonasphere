# shared — протокол bitvm

Единственный код, который видят и sona, и bitvm: команды bitvm, бинарный кодек, текстовая форма
(asm/disasm) и транспорт поверх Unix `SOCK_SEQPACKET`.

- Никакого DSP, аудио, ALSA и никакой грамматики sona.
- Любое изменение формата идёт вместе с версией протокола и `docs/protocol.md`.

Заголовки — `include/bitvm/proto/`, пространство имён `bitvm::proto`, meson-зависимость
`bitvm_proto_dep` (статическая библиотека `bitvm-proto`). Спецификация — `docs/protocol.md`,
архитектурный контекст — `docs/architecture.md`, §4.

| Модуль | Что внутри |
|--------|------------|
| `types` | ID, перечисления, константы и пределы |
| `messages` | структуры сообщений, команд, результатов и событий |
| `codec` | `encode` / `decode`, разбор по командам для дизассемблера |
| `validate` | статическая проверка бандла (уровень 1) |
| `graphdef` | GraphDef: структура, кодек, каноническая форма, валидатор |
| `catalog` | каталог юнитов v0 и `catalog_hash()` |
| `text` | ассемблер и дизассемблер сообщений и `.bvg` |
| `transport` | путь сокета, `listen` / `connect`, отправка и приём одного сообщения |

## Тесты и фаззинг

```bash
meson test -C build --suite shared
```

Golden-файлы лежат в `tests/data/`. Чтобы пересоздать `*.hex` и `*.canonical.*` после
осознанного изменения формата, запусти нужный тест с `BITVM_UPDATE_GOLDEN=1` и просмотри diff.
Значение `catalog_hash()` зафиксировано в `tests/catalog_hash.cpp`, текст каталога — в
`tests/data/catalog.golden`.

Фаззинг — только clang и libFuzzer:

```bash
CXX=clang++ meson setup build-fuzz -Dfuzz=true -Db_sanitize=address,undefined
meson test -C build-fuzz --suite fuzz
```

Для долгого прогона первым аргументом идёт рабочий каталог корпуса, вторым — зерна:

```bash
build-fuzz/shared/fuzz_decode /tmp/corpus shared/tests/data/corpus/decode -max_total_time=60
```

Вход, на котором фаззер упал, кладётся в `tests/data/corpus/<цель>/` как регрессионный.
