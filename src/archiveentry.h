// SPDX-FileCopyrightText: 2026 LingmoOS Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Reading, extracting and writing archives through libarchive: the part of
// the archive manager with no user interface in it.
namespace archivemanager {

// One entry of an archive as it is listed.
struct Entry
{
    // The path exactly as the archive stores it.
    std::string path;
    // The path made safe for extraction: relative, no "." or ".." part.
    // Empty when the stored path is unsafe.
    std::string relativePath;
    // The last part of the path, for the list.
    std::string name;
    bool directory = false;
    bool symlink = false;
    bool hardlink = false;
    // The stored path is absolute or climbs out with "..": it is listed but
    // never extracted.
    bool unsafe = false;
    uint64_t size = 0;
    // Seconds since the epoch, 0 when the archive does not say.
    int64_t mtime = 0;
    uint32_t mode = 0;
    std::string linkTarget;
};

struct Listing
{
    bool ok = false;
    std::string error;
    // libarchive's names for the format and the outermost filter.
    std::string formatName;
    std::string filterName;
    std::vector<Entry> entries;
    // The uncompressed size of every regular entry.
    uint64_t totalSize = 0;
    bool encrypted = false;
    // Entries whose stored path is unsafe.
    size_t unsafePaths = 0;
};

struct Progress
{
    uint64_t completed = 0;
    uint64_t total = 0;
    // The entry being worked on, its path as the list shows it.
    std::string current;
};

struct ExtractOptions
{
    bool overwrite = true;
    // Symbolic links are restored only when their target stays inside the
    // destination; absolute and escaping links are skipped.
    bool restoreSymlinks = true;
};

struct ExtractResult
{
    bool ok = false;
    bool cancelled = false;
    std::string error;
    size_t files = 0;
    size_t directories = 0;
    size_t links = 0;
    size_t skipped = 0;
    uint64_t bytes = 0;
};

enum class CreateFormat {
    Zip,
    Tar,
    TarGz,
};

struct CreateResult
{
    bool ok = false;
    bool cancelled = false;
    std::string error;
    size_t files = 0;
    size_t directories = 0;
    uint64_t bytes = 0;
};

using ProgressCallback = std::function<void(const Progress&)>;

// Lists the entries of the archive at `path`; never throws.
Listing listArchive(const std::string& path);

// Extracts the entries `selected` (indices into `entries`, which must be the
// listing of `archivePath`; empty selects nothing) into `destination`, which
// must exist. Progress reports are made from the calling thread; `cancel`,
// when set, stops the extraction at the next block.
ExtractResult extractArchive(const std::string& archivePath, const std::vector<Entry>& entries,
                             const std::vector<size_t>& selected, const std::string& destination,
                             const ExtractOptions& options, const ProgressCallback& progress = {},
                             const std::atomic<bool>* cancel = nullptr);

// Writes `sources` (files and folders, recursively) into a new archive at
// `archivePath`, replacing what was there.
CreateResult createArchive(const std::string& archivePath, const std::vector<std::string>& sources,
                           CreateFormat format, const ProgressCallback& progress = {},
                           const std::atomic<bool>* cancel = nullptr);

// True when `raw` names an entry that is safe to extract; `relative` is the
// normalised relative path and `explicitDirectory` is set for a trailing
// slash. False for absolute paths, any ".." part, NUL bytes and empty paths.
bool normaliseEntryPath(std::string_view raw, std::string* relative, bool* explicitDirectory);

// A short label such as "ZIP" or "TAR.GZ" for the list's format chip.
std::string formatLabel(const std::string& archivePath, const std::string& libarchiveFormat,
                        const std::string& filterName);

// The format createArchive writes for a file name, when its extension names
// one it can write.
bool createFormatForPath(const std::string& path, CreateFormat* format);

// The save-filter patterns and labels the file dialogs offer.
std::string archiveNameFilter();
std::string archiveMimeTypes();

} // namespace archivemanager
