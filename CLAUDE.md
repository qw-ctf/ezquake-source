# CLAUDE.md - ezQuake Development Guide

## Project Overview

ezQuake is a modern QuakeWorld client written primarily in C (gnu89 standard). It supports Windows, Linux, macOS, and FreeBSD via SDL2. Licensed under GPLv2.

## Repository Structure

- `src/` - All source code (flat directory, no subdirectories except `glsl/` and `qwprot/`)
- `src/glsl/` - GLSL shaders for modern OpenGL renderer
- `cmake/` - CMake modules and vcpkg triplets
- `dist/` - Distribution/packaging files
- `misc/` - Miscellaneous resources
- `help_*.json` - In-game help data files

### Source File Naming Conventions

- `cl_*` - Client-side code
- `sv_*` - Server-side code
- `r_*` - Renderer-agnostic rendering code
- `gl_*` - Shared OpenGL code
- `glc_*` - Classic (immediate mode) OpenGL renderer
- `glm_*` - Modern OpenGL renderer
- `vk_*` - Vulkan renderer (WIP)
- `hud_*` - HUD elements
- `EX_*` - Server browser
- `ez_*` - UI controls
- `vx_*` - Visual effects (coronas, particles, tracker)
- `vfs_*` - Virtual filesystem backends
- `sys_*` / `cd_*` / `in_*` - Platform-specific code

## Build System

CMake-based build. See `BUILD.md` for full details.

### Quick Build (Linux, dynamic linking)

```sh
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j $(nproc)
```

Or using presets:

```sh
cmake --preset dynamic
cmake --build --preset dynamic-release
```

### Static Build (uses vcpkg)

```sh
./bootstrap.sh
cmake --preset static
cmake --build build-static --config Release
```

### Key CMake Options

```
RENDERER_MODERN_OPENGL   - Modern OpenGL renderer (default: ON)
RENDERER_CLASSIC_OPENGL  - Classic OpenGL renderer (default: ON)
USE_SYSTEM_LIBS          - Use system libraries vs vcpkg (default: ON)
ENABLE_LTO               - Link Time Optimization (default: ON)
```

### Dependencies

Required: OpenGL, SDL2, Expat, JPEG, Jansson, MiniZip, PCRE2, PNG, SndFile, cURL, zlib
Optional: FreeType, Speex, SpeexDSP

### Adding New Source Files

Source files in `CMakeLists.txt` are categorized by subsystem (client, server, common, sys, etc.). Add new files to the appropriate category.

## Coding Style

- C language, gnu89 standard (`-std=gnu89`)
- Non-MSVC builds use `-Wall` with specific warnings promoted to errors:
  - `-Werror=format`, `-Werror=strict-prototypes`, `-Werror=old-style-definition`
  - `-Werror=unused-function` and `-Werror=unused-variable` in Release builds
- Old-style C function definitions are used throughout (K&R style prototypes are errors)
- Tabs for indentation in existing code

## Commit Message Convention

Commits use a prefix convention: `SUBSYSTEM: description` (e.g., `VX_TRACKER: Add r_tracker_weapon_first`, `CONFIG: enable cl_pext_lagteleport by default`, `BUG: fix potential race condition`). Common prefixes include subsystem names in caps, `BUG`, `PIPELINE`, `DEMO`, `SDL`, etc.
