# lingmo-archive-manager

A native archive manager for Lingmo OS, based on libarchive.

## Features

- Open zip, tar, tar.gz, 7z and rar archives.
- List paths, sizes and modified times.
- Extract every entry with safe path handling.
- Light and dark themes.

## Build

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/lingmo-sdk
cmake --build build
sudo cmake --install build
```

## Screenshots

![Light theme](docs/screenshot-2026-11-02-light.png)
![Dark theme](docs/screenshot-2026-11-02-dark.png)
