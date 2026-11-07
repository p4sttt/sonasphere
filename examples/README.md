# examples

Примеры композиций `.sona` появятся после разработки языка (`docs/architecture.md`).

`offline/ping.bvs` и `offline/ping.bvg` — партитура и DSP-граф bitvm, не требующие sona:

```bash
build/bitvm/bitvm-render --duration 2 examples/offline/ping.bvs build/ping.wav
```
