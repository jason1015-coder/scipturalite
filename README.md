# Scriptura Lite

A lightweight Qt and Rust code editor — Qt for the UI, Rust for safe backend services.

[![Build & Deploy Scriptura](https://github.com/jason1015-coder/scipturalite/actions/workflows/build.yml/badge.svg)](https://github.com/jason1015-coder/scipturalite/actions/workflows/build.yml)
[![Run Unit Tests](https://github.com/jason1015-coder/scipturalite/actions/workflows/test.yml/badge.svg)](https://github.com/jason1015-coder/scipturalite/actions/workflows/test.yml)

> **Status:** pre-1.0, and in active development. Expect bugs and occasional broken features.

---

## Requirements

- **Qt 6** (with Widgets, Network, Sql, and LinguistTools modules)
- **CMake 3.16+**
- **C++17 compiler** (GCC, Clang, MSVC)
- **Rust toolchain** — install via [rustup](https://rustup.rs):

```bash
curl --proto =https --tlsv1.2 -sSf https://sh.rustup.rs | sh
```

---

## Building

```bash
./run.sh
```

`./run.sh` builds the Rust backend and the Qt UI incrementally, then launches the app.

Or directly:

```bash
./cmake-build-Debug/scriptura
```

---

## Contribute

See [CONTRIBUTING.md](CONTRIBUTING.md) for development setup, coding standards, and the test gate.