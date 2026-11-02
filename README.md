# AviUtl64 <!-- omit in toc -->

AviUtl64 is a decompilation of AviUtl (**AviUtl version 1.10 by KENくん**, the japanese ytpmv software).

The current progress is around ~10-20% complete. Basic features are working but there will be bugs and stuff.

> [!CAUTION]
> This is not stable yet, so please make backups of your projects and aviutl folder.

## Table of Contents <!-- omit in toc -->
- [Plugins](#plugins)
  - [Plugin development](#plugin-development)
  - [Porting Plugins](#porting-plugins)
- [Building](#building)
  - [Requirements](#requirements)
  - [Instructions](#instructions)


# Plugins

Plugin support is very limited, you can run 32-bit plugin because we made a bridge.
so without 64-bit plugin, the performance will be the same as original aviutl or worse.

Place `.auf` and `.auo` plugins in `plugins/` next to `reaviutl.exe` (or in the same folder as `reaviutl.exe`).

## Plugin development

TBA

## Porting Plugins

TBA

# Building

To build AviUtl64, please see requirements down below.

## Requirements

1. Computer that's running Windows 10 or 11
2. [Git for Windows](https://git-scm.com/install/windows)
3. Visual Studio 2019 (or newer) with "Desktop development with C++"
4. CMake 3.15+

## Instructions

TBA