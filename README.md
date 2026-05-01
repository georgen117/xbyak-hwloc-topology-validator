# cpu-topology-test

A cross-platform C++ test that validates CPU topology data reported by
[xbyak](https://github.com/herumi/xbyak) (`xbyak_util.h`) against the same
data from [hwloc](https://github.com/open-mpi/hwloc).

The test covers:

- Logical CPU (PU) count
- Physical core count
- Cache-line size
- P-core / E-core / LP E-core counts (hybrid CPU systems)
- Number of physical cores sharing each cache level per core type
- Per-CPU cache sizes (L1d, L2, L3) for every logical CPU

hwloc is always built as a **static library** from the git submodule — no
system-installed hwloc is required.

---

## Repository layout

```
cpu-topology-test/
  extern/
    xbyak/          # git submodule  (header-only, no build required)
    hwloc/          # git submodule  (built from source)
  cmake/
    ConfigureHwloc.cmake   # Linux autotools helper
  test/
    test_topology.cpp
  CMakeLists.txt
  README.md
```

---

## Prerequisites

| Platform | Requirements |
|----------|-------------|
| Windows  | Visual Studio 2019 or 2022 (MSVC), CMake >= 3.16 |
| Linux    | GCC or Clang, CMake >= 3.16, `autoconf automake libtool` |

Install the Linux autotools prerequisites with:

```bash
# Debian / Ubuntu
sudo apt install autoconf automake libtool

# Fedora / RHEL
sudo dnf install autoconf automake libtool
```

---

## Clone and initialise submodules

```bash
git clone https://github.com/georgen117/cpu-topology-test.git
cd cpu-topology-test
git submodule update --init --recursive
```

If you already have the repo and just need to pull updates to the submodules:

```bash
git submodule update --recursive --remote
```

---

## Build

### Windows (Visual Studio)

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

### Linux

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

---

## Run the test

Run the test executable directly:

```bat
rem Windows
build\Release\test_topology.exe
```

```bash
# Linux
./build/test_topology
```

Or use CTest:

```bash
# Windows (from project root)
ctest --test-dir build -C Release

# Linux (from project root)
ctest --test-dir build
```

A passing run ends with a summary line such as:

```
60 passed, 0 failed, 0 warnings
```

---

## How it works

The test instantiates `Xbyak::util::CpuTopology` (from `xbyak_util.h`) and an
`hwloc_topology_t` side by side, then compares corresponding values using
`check_eq()` assertions.  On hybrid Intel CPUs the test classifies each core
as P-core, E-core, or LP E-core:

- **P-core** — highest efficiency kind (hwloc) / non-Efficient flag (xbyak)
- **E-core** — lower efficiency kind with an L3 cache ancestor
- **LP E-core** — lower efficiency kind with *no* L3 cache (xbyak: Efficient
  flag + L3 size == 0)
