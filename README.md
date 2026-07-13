# Easy Setup

A small native Windows app for setting up a fresh PC in a few clicks: pick installers,
flip common Windows settings, and run quick maintenance tools — all from one dark,
Fluent-styled window.

Single-file C++ (Win32 + Direct2D), no runtime dependencies.

## Build

Requires Visual Studio (with the Desktop C++ workload).

```
Build.bat
```

This produces `EasySetup.exe`. Run it with `Run.bat` or by double-clicking the exe.

## Features

- **Installers** — search and multi-select apps, then install the latest version of
  each via `winget`. Shows real brand icons, live per-app install status, and flags
  apps you already have installed.
- **Windows Settings** — toggle common taskbar/Explorer/developer tweaks directly via
  the registry.
- **Misc** — startup app management, disk cleanup, system consoles, and other
  one-click maintenance tools.

## Files

| File | Purpose |
|---|---|
| `EasySetup.cpp` | Application source (single file) |
| `icons_data.h` | Embedded brand icon PNGs (generated) |
| `app.ico` / `app.rc` | App icon and Win32 resource script |
| `Build.bat` | Compiles with MSVC |
| `Run.bat` | Builds (if needed) and launches |
