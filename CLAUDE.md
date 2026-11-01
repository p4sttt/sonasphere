# Sonasphere

Среда live coding музыки в духе Sonic Pi, Strudel и TidalCycles. Интерфейс — CLI. C++23, meson.
Перед работой прочитай `docs/architecture.md`: там решения, границы компонентов и дорожная карта.
Работа идёт по этапам из §9 — одна сессия, один этап.

## Компоненты

- `shared/` — протокол bitvm: команды, кодек, текстовая форма, транспорт. Его видят оба процесса.
- `bitvm/` — аудиодвижок и демон `bitvmd`: звук, точное время, исполнение DSP-графов.
- `sona/` — язык, паттерны, темп, REPL. Транслирует код в поток команд bitvm.
- `stdlib/` — стандартная библиотека на языке sona. `examples/` — примеры `.sona`.

## Жёсткие правила

- sona зависит только от `bitvm_proto_dep`. Никогда не добавляй в sona `bitvm_dep` или include движка.
- bitvm не знает про sona: никаких ключевых слов, темпа, тактов, имён сэмплов, файлов `.sona`.
  Время в bitvm — только абсолютное (`CLOCK_MONOTONIC`, нс).
- Audio thread: без аллокаций, мьютексов, ввода-вывода и исключений. Всё готовит control thread.
- Формат протокола меняется только вместе с его версией и `docs/protocol.md`.
- Синтаксис sona ещё проектируется. Не придумывай его без согласования с пользователем; когда
  появится `docs/language.md`, опирайся на него.
- Архитектурное решение поменялось — обнови `docs/architecture.md` (таблица решений в §3).

## Команды

```bash
meson setup build                                    # один раз
meson compile -C build
meson test -C build                                  # все тесты; --suite shared|bitvm|sona
meson setup build-asan -Db_sanitize=address,undefined
```

clangd находит `build/compile_commands.json` сам.

## Конвенции

- Типы — `PascalCase`; функции и переменные — `snake_case`; поля класса — `name_`;
  константы — `kName`. Файлы — `snake_case.hpp` / `.cpp`.
- Пространства имён: `bitvm::proto`, `bitvm`, `sona`.
- Ошибки на границах модулей — `std::expected<T, Error>`. Код audio thread — `noexcept`.
- Форматирование — `.clang-format`.
- Комментарии в коде и сообщения коммитов — на английском в нижнем регистре только ascii символы (то есть нет длинных тире, используй "-", угловых ковычек "«" и тд), документация — на русском.
- Новый тест — файл `<name>.cpp` в `<компонент>/tests/` и имя в списке в `tests/meson.build`.
- Пока в компоненте нет `.cpp`, его библиотека — `declare_dependency` с одними заголовками.
  С первым исходником она становится `static_library` (см. комментарий в его `meson.build`).
- Текст коммитов имеет формат [feat/fix/chrone/docs]: <short message>\n <body> без приписок co-authored
