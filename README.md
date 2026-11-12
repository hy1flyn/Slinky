<p align="center">
  <img src="assets/ss.png" alt="Open Slinky Banner" width="350" />
  <br>
  <font size="6"><b>Open Slinky</b></font>
  <br>
  <font size="3">by Skid Solutions</font>
  <br>
  <font size="2">credits to cat;
</p>

---

<p align="center">
  <a href="https://discord.gg/AEwatvnvSf">https://discord.gg/AEwatvnvSf</a>
</p>

<p align="center">
  <img src="assets/jak.jpg" alt="java hooks" height="150" />
  <img src="assets/hack.jpg" alt="unload" height="150" />
  <img src="assets/deobf2.png" alt="symbol recovery" height="150" />
  <img src="assets/deobf.jpg" alt="readable names" height="150" />
</p>

---

## About

Open Slinky is a C++ implementation of the Slinky client for Minecraft 1.8.9.
The client is injected into the game process as a native DLL; it hooks the game
at the Java bytecode level through JVMTI, dispatches events to the module
system, and renders its interface through NanoVG.

---

## How to build

### Prerequisites

- **LLVM** — clang-cl, lld-link, llvm-rc and llvm-mt on `PATH`
- **CMake** 3.20 or newer
- **Ninja**
- **Windows x64**
- **Java Development Kit 8** — runtime for Minecraft 1.8.9; not required to
  compile the DLL

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

Output:

- `build/slinky.dll`

### Or use the build script

`build.ps1` finds the toolchain, builds the DLL, builds the injector and copies
`Slinky.exe` to `dist\`:

```powershell
.\build.ps1                 # client + injector
.\build.ps1 -Clean          # wipe build\ first
.\build.ps1 -Client         # DLL only
.\build.ps1 -Injector       # injector only (uses the existing DLL)
.\build.ps1 -Jobs 8         # limit parallelism
```

`build.bat` does the same from Explorer.

The script looks for the tools on `PATH` first, then in their usual install
locations (`C:\Program Files\LLVM\bin`, `C:\Program Files\CMake\bin`,
`C:\Strawberry\c\bin`, ...). The injector needs MinGW-w64 `g++`.

### Running

Inject `slinky.dll` into the running Minecraft 1.8.9 process with
`LoadLibrary`. Start-up runs on its own thread; the client interface is opened
with its GUI key. The last interface tab unloads the client: it saves the
settings, removes the Java hooks and frees the library.

The injector in `injector/` bundles the library and does this for you:

```powershell
Slinky.exe            # find Minecraft and inject
Slinky.exe --list     # list Java processes
Slinky.exe --help     # all options
```

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

## Notes

- Settings and profiles are stored in `~/.slinky` (`settings`, `profiles/`).
- `src/data/*.s` and `src/platform/win/chkstk.s` are data and compiler-support
  sections, not client code.

## Credits

- Original client: Slinky

## Legal & disclaimers

- **Affiliation:** This is an independent development. It is **NOT AN OFFICIAL
  SLINKY PRODUCT**, is not approved by or associated with the original authors,
  and is not affiliated with Mojang or Zelix Pty Ltd.
- **Assets & code:** The `src/data` tables are the client's own data required for
  linking; all third-party assets and code belong to their respective owners.
- **Purpose:** research and preservation. Use at your own risk.
