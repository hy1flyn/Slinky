
# Slinky
## build

### Prerequisites

- **LLVM** — clang-cl, lld-link, llvm-rc and llvm-mt on `PATH`
- **CMake** 3.20 or newer
- **Ninja**

### 1. Configure

```powershell
cmake -S . -B build -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-windows-clang-cl.cmake
```

If Ninja is not on `PATH`, add
`-DCMAKE_MAKE_PROGRAM=C:/path/to/ninja.exe`.

### 2. Build

```powershell
cmake --build build
```

### Or use the build script

`build.ps1` finds the toolchain, builds the DLL, builds the injector and copies
`Slinky.exe` to `dist\`:

---

## Project layout

```
src/engine/         hooks, JNI/JVMTI, GUI, module manager, math, shared services
src/logic/          per-module behaviour (.cpp) and its descriptor callbacks / settings page (.h)
src/platform/       Windows layer, GL loader, NanoVG integration
src/data/           data tables (fonts, strings, descriptors) - required to link
include/slinky/     public headers
injector/           terminal loader and DLL injector
third_party/        NanoVG and GL headers
```



## Legal & disclaimers

- **Affiliation:** This is an independent development. It is **NOT AN OFFICIAL
  SLINKY PRODUCT**, is not approved by or associated with the original authors,
  and is not affiliated with Mojang or Zelix Pty Ltd.
- **Assets & code:** The `src/data` tables are the client's own data required for
  linking; all third-party assets and code belong to their respective owners.
- **Purpose:** research and preservation. Use at your own risk.
