# examples

Примеры композиций `.sona`. Пусто, пока не спроектирован язык (`docs/architecture.md`, сессия L1).

`offline/ping.bvs` и `offline/ping.bvg` — партитура и DSP-граф bitvm, не требующие sona:

```bash
build/bitvm/bitvm-render --duration 2 examples/offline/ping.bvs build/ping.wav
```
