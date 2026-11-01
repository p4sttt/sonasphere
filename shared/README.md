# shared — протокол bitvm

Единственный код, который видят и sona, и bitvm: команды bitvm, бинарный кодек, текстовая форма
(asm/disasm) и транспорт поверх Unix `SOCK_SEQPACKET`.

- Никакого DSP, аудио, ALSA и никакой грамматики sona.
- Любое изменение формата идёт вместе с версией протокола и `docs/protocol.md`.

Заголовки — `include/bitvm/proto/`, пространство имён `bitvm::proto`, meson-зависимость
`bitvm_proto_dep`. Подробности — `docs/architecture.md`, §4.
