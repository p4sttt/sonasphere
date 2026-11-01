# sona — музыкальная мысль

Язык, паттерны, темп, сессия REPL и исполнение файлов `.sona`. Переводит код автора в поток команд
bitvm с временными метками.

Про аудио не знает ничего: зависит только от `bitvm_proto_dep`, движок bitvm ей не виден.

- `include/sona/` — API libsona для приложения и тестов
- `src/` — lang, pattern, graph, session, sched, lower, client, repl (каталоги появляются вместе с кодом)
- `app/` — исполняемый `sona`
- `tests/` — gtest; новый тест = имя в списке в `tests/meson.build`

Синтаксис языка ещё не спроектирован (сессия L1). Подробности — `docs/architecture.md`, §6.
