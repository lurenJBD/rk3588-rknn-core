# Documentation Index

The repository-root `README.md` is the short overview. Detailed material for
first-time users lives here.

| Document | Contents |
|---|---|
| [build-and-install.md](build-and-install.md) | Dependencies, build, install, load/verify, clean unload |
| [compatibility.md](compatibility.md) | Reference platform, tested boards/kernels, supported SoCs |
| [validation.md](validation.md) | Acceptance test flow and how to reproduce it |
| [scores.md](scores.md) | Performance scores for the toolkit demos |
| [ling-3-tiny-rknn.md](ling-3-tiny-rknn.md) | Ling-3.0-tiny W4A8 LLM result |
| [module-parameters.md](module-parameters.md) | Module parameters and debugfs nodes |
| [memory-and-dmc.md](memory-and-dmc.md) | Memory bandwidth, mainline DMC facts, and their impact |

Labels used in [compatibility.md](compatibility.md):

- `verified` — measured on the maintainer's reference board.
- `user report` — reported by a third party; not reproduced by the maintainer.

The on-board test cases and preparation scripts live in
[`../test-suite/`](../test-suite/). Models and demo binaries are staged on
demand by `test-suite/prepare-test-assets.sh`.
