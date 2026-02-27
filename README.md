# Htop Widget

A process monitor showing per-core CPU bars, a memory bar, and a live process table sorted by CPU usage.

## Requirements

- Linux
- Qt 6.2+ (Widgets)
- CMake 3.21+
- C++20 compiler
- [`widget-sdk`](https://github.com/duh-dashboard/widget-sdk) installed

## Build

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=~/.local
cmake --build build
cmake --install build --prefix ~/.local
```

The plugin installs to `~/.local/lib/dashboard/plugins/`.

## Notes

Reads the following Linux-specific interfaces:

| Data | Source |
|---|---|
| Per-core CPU usage | `/proc/stat` (delta sampling) |
| Memory usage | `/proc/meminfo` |
| Process list | `/proc/<pid>/stat` |

## License

GPL-3.0-or-later — see [LICENSE](LICENSE).
