# ADR 0001: Project Initialization

## Status

Superseded by [ADR 0002: Migrate Build System to CMake + Conan](0002-migrate-to-cmake-conan.md)

The Build System, Version Control, and Consequences sections regarding xmake are superseded. Other decisions (C++20, multi-process architecture, strategy API design, static library strategy, project structure) remain in effect.

## Context

We are initializing the dztrader project, a high-performance quantitative trading system. Key decisions need to be made about the build system, language standard, architecture, and project structure.

## Decision

1. **Build System**: xmake with built-in package manager
   - xmake provides simple Lua-based build configuration
   - Built-in package manager replaces external dependency management
   - Closed-source broker SDKs managed via `third_party/`

2. **Language Standard**: C++20
   - Enables concepts, ranges, coroutines, std::format, std::span
   - Well-supported by MSVC 2022 and GCC 12+

3. **Architecture**: Multi-process with shared memory IPC
   - One strategy per process for crash isolation
   - Dual-channel shared memory for low-latency communication
   - Master process is minimal for maximum stability

4. **Strategy API**: Three-layer design (C + C++ merged, Python separate)
   - Pure C interface for binary compatibility
   - C++ wrapper for convenient development
   - Python bindings via pybind11

5. **Static Library Strategy**: Internal libraries statically linked
   - Avoids DLL hell and version conflicts
   - Strategy API dynamically linked only for strategy processes

6. **Project Structure**: Organized by component (libs/, apps/, tests/)
   - Libraries in `libs/` with include/src/tests subdirectories
   - Applications in `apps/` with gateway subdirectories
   - Integration tests in `tests/`

7. **Version Control**: Conventional commits, xmake.lua `set_version` as single source of truth
   - version.h.in template processed by xmake to generate version.h

## Consequences

- Build system requires xmake knowledge
- C++20 limits compiler support to MSVC 2022+ and GCC 12+
- Multi-process architecture increases complexity but provides fault isolation
- Static linking increases binary size but improves reliability
