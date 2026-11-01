# bitvm — звук, время, вычисления

Аудиодвижок и демон `bitvmd`. Принимает поток команд из `shared/`, исполняет DSP-графы с точностью
до сэмпла и отдаёт звук в бэкенд (ALSA, offline).

Про язык sona не знает ничего: ни ключевых слов, ни темпа, ни имён сэмплов, ни файлов `.sona`.

- `include/bitvm/` — API движка для bitvmd, утилит и тестов
- `src/` — engine, graph, dsp, backend, io, server (каталоги появляются вместе с кодом)
- `bitvmd/` — демон
- `tools/` — утилиты вроде `bitvmctl`; зависят только от `bitvm_proto_dep`
- `tests/` — gtest; новый тест = имя в списке в `tests/meson.build`

Подробности — `docs/architecture.md`, §5.
