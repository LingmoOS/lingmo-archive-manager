// SPDX-FileCopyrightText: 2026 LingmoOS Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "archiveentry.h"

#include <archive.h>
#include <archive_entry.h>

#include "base/i18n.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace fs = std::filesystem;
using Lingmo::i18n::format;
using Lingmo::i18n::tr;

namespace archivemanager {
namespace {

std::string errorText(struct ::archive* reader)
{
    if (!reader)
        return tr("Unknown archive error");
    if (const char* message = archive_error_string(reader))
        return message;
    const int code = archive_errno(reader);
    if (code != 0)
        return std::strerror(code);
    return tr("Unknown archive error");
}

// libarchive's message for an archive that wants a password, in the words
// of the formats it supports.
std::string encryptionMessage(struct ::archive* reader)
{
    const bool encrypted = archive_read_has_encrypted_entries(reader) > 0;
    if (encrypted)
        return tr("This archive is password-protected; encrypted archives are not supported.");
    if (const char* message = archive_error_string(reader)) {
        const std::string text = message;
        std::string lower = text;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (lower.find("passphrase") != std::string::npos || lower.find("password") != std::string::npos
            || lower.find("encrypt") != std::string::npos)
            return tr("This archive is password-protected; encrypted archives are not supported.");
    }
    return errorText(reader);
}

void supportReadFormats(struct ::archive* reader)
{
    // Not format_all: formats such as mtree name files elsewhere on disk.
    archive_read_support_format_zip(reader);
    archive_read_support_format_7zip(reader);
    archive_read_support_format_rar(reader);
    archive_read_support_format_rar5(reader);
    archive_read_support_format_tar(reader);
    archive_read_support_format_cpio(reader);
    archive_read_support_format_iso9660(reader);
    archive_read_support_format_empty(reader);
    // Raw bids lowest, so it only takes a lone compressed file no archive
    // format claims, such as notes.txt.gz.
    archive_read_support_format_raw(reader);
}

void supportReadFilters(struct ::archive* reader)
{
    archive_read_support_filter_gzip(reader);
    archive_read_support_filter_bzip2(reader);
    archive_read_support_filter_compress(reader);
    archive_read_support_filter_lzip(reader);
    archive_read_support_filter_lzma(reader);
    archive_read_support_filter_xz(reader);
    archive_read_support_filter_zstd(reader);
}

// A reader closed with the object.
using Reader = std::unique_ptr<struct ::archive, decltype(&archive_read_free)>;

Reader openReader(const std::string& path, std::string* error)
{
    Reader reader(archive_read_new(), &archive_read_free);
    if (!reader) {
        *error = tr("Could not allocate memory for the archive.");
        return reader;
    }
    supportReadFormats(reader.get());
    supportReadFilters(reader.get());
    if (archive_read_open_filename(reader.get(), path.c_str(), 64 * 1024) != ARCHIVE_OK) {
        *error = encryptionMessage(reader.get());
        reader.reset();
    }
    return reader;
}

std::vector<std::string_view> pathParts(std::string_view path)
{
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        const size_t end = slash == std::string_view::npos ? path.size() : slash;
        parts.push_back(path.substr(start, end - start));
        if (slash == std::string_view::npos)
            break;
        start = slash + 1;
    }
    return parts;
}

std::string baseName(std::string_view path)
{
    while (!path.empty() && path.back() == '/')
        path.remove_suffix(1);
    const size_t slash = path.rfind('/');
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

std::string parentOf(std::string_view relative)
{
    const size_t slash = relative.rfind('/');
    return slash == std::string_view::npos ? std::string() : std::string(relative.substr(0, slash));
}

std::string joinPath(const std::string& base, std::string_view relative)
{
    std::string path = base;
    if (!path.empty() && path.back() == '/')
        path.pop_back();
    path += '/';
    path += relative;
    return path;
}

bool canonicalDirectory(const std::string& path, std::string* canonical, std::string* error)
{
    char* resolved = ::realpath(path.c_str(), nullptr);
    if (!resolved) {
        *error = format(tr("The destination folder cannot be used: {error}"), {{"error", std::strerror(errno)}});
        return false;
    }
    const std::string result = resolved;
    std::free(resolved);
    struct stat st;
    if (::stat(result.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        *error = tr("The destination is not a folder.");
        return false;
    }
    *canonical = result;
    return true;
}

// Creates the folders of `relative` under `base` one at a time, refusing any
// existing symbolic link on the way so an archive cannot be redirected out of
// the destination.
bool ensureDirectoryWithin(const std::string& base, std::string_view relative, mode_t mode, std::string* error)
{
    std::string current = base;
    for (const std::string_view part : pathParts(relative)) {
        if (part.empty() || part == ".")
            continue;
        current = joinPath(current, part);
        struct stat st;
        if (::lstat(current.c_str(), &st) == 0) {
            if (S_ISLNK(st.st_mode)) {
                *error = format(tr("A symbolic link is in the way: {path}"), {{"path", current}});
                return false;
            }
            if (!S_ISDIR(st.st_mode)) {
                *error = format(tr("A file is in the way: {path}"), {{"path", current}});
                return false;
            }
            continue;
        }
        if (errno != ENOENT) {
            *error = format(tr("Could not create {path}: {error}"), {{"path", current}, {"error", std::strerror(errno)}});
            return false;
        }
        if (::mkdir(current.c_str(), mode_t(mode | S_IRWXU)) != 0 && errno != EEXIST) {
            *error = format(tr("Could not create {path}: {error}"), {{"path", current}, {"error", std::strerror(errno)}});
            return false;
        }
    }
    return true;
}

// Opens a file for extracted data, replacing a regular file when asked.
// A symbolic link or folder where the file should go is left alone.
int openFileForExtract(const std::string& path, mode_t mode, bool overwrite)
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
        if (fd >= 0)
            return fd;
        if (errno != EEXIST)
            return -1;
        if (!overwrite) {
            errno = EEXIST;
            return -1;
        }
        struct stat st;
        if (::lstat(path.c_str(), &st) != 0)
            return -1;
        if (!S_ISREG(st.st_mode)) {
            errno = EEXIST;
            return -1;
        }
        if (::unlink(path.c_str()) != 0)
            return -1;
    }
    return -1;
}

bool writeAll(int fd, const char* data, size_t size)
{
    size_t written = 0;
    while (written < size) {
        const ssize_t result = ::write(fd, data + written, size - written);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        written += size_t(result);
    }
    return true;
}

bool isCancelled(const std::atomic<bool>* cancel)
{
    return cancel && cancel->load(std::memory_order_relaxed);
}

// Whether a symbolic link's target stays inside the destination: relative,
// and its ".." parts never climb above it.
bool linkStaysInside(std::string_view relativePath, const std::string& target)
{
    if (target.empty() || target.front() == '/' || target.find('\0') != std::string::npos)
        return false;
    if (target.size() >= 2 && std::isalpha(static_cast<unsigned char>(target[0])) && target[1] == ':')
        return false;
    std::vector<std::string> stack;
    for (const std::string_view part : pathParts(parentOf(relativePath))) {
        if (!part.empty() && part != ".")
            stack.emplace_back(part);
    }
    for (const std::string_view part : pathParts(target)) {
        if (part.empty() || part == ".")
            continue;
        if (part == "..") {
            if (stack.empty())
                return false;
            stack.pop_back();
            continue;
        }
        stack.emplace_back(part);
    }
    return true;
}

void applyTimes(const std::string& path, int64_t modified, bool follow)
{
    if (modified <= 0)
        return;
    const struct timespec times[2] = {{0, UTIME_OMIT}, {time_t(modified), 0}};
    ::utimensat(AT_FDCWD, path.c_str(), times, follow ? 0 : AT_SYMLINK_NOFOLLOW);
}

std::string lowerExtension(const std::string& path)
{
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    const size_t slash = lower.rfind('/');
    const std::string name = slash == std::string::npos ? lower : lower.substr(slash + 1);
    return name;
}

bool endsWith(const std::string& text, std::string_view suffix)
{
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string upperCopy(std::string_view text)
{
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    return result;
}

uint64_t entrySize(struct archive_entry* entry)
{
    const la_int64_t size = archive_entry_size(entry);
    return size > 0 ? uint64_t(size) : 0;
}

} // namespace

bool normaliseEntryPath(std::string_view raw, std::string* relative, bool* explicitDirectory)
{
    if (relative)
        relative->clear();
    if (explicitDirectory)
        *explicitDirectory = false;
    if (raw.empty() || raw.find('\0') != std::string_view::npos)
        return false;
    if (raw.front() == '/')
        return false;
    if (raw.size() >= 2 && std::isalpha(static_cast<unsigned char>(raw[0])) && raw[1] == ':')
        return false;
    std::vector<std::string> kept;
    for (const std::string_view part : pathParts(raw)) {
        if (part.empty() || part == ".")
            continue;
        if (part == "..")
            return false;
        kept.emplace_back(part);
    }
    if (kept.empty())
        return false;
    std::string result;
    for (const std::string& part : kept) {
        if (!result.empty())
            result += '/';
        result += part;
    }
    if (relative)
        *relative = std::move(result);
    if (explicitDirectory)
        *explicitDirectory = raw.back() == '/';
    return true;
}

Listing listArchive(const std::string& path)
{
    Listing listing;
    std::string error;
    Reader reader = openReader(path, &error);
    if (!reader) {
        listing.error = std::move(error);
        return listing;
    }
    struct archive_entry* entry = nullptr;
    while (true) {
        const int status = archive_read_next_header(reader.get(), &entry);
        if (status == ARCHIVE_EOF)
            break;
        if (status < ARCHIVE_WARN) {
            listing.error = encryptionMessage(reader.get());
            listing.entries.clear();
            return listing;
        }
        Entry item;
        const char* name = archive_entry_pathname_utf8(entry);
        if (!name)
            name = archive_entry_pathname(entry);
        item.path = name ? name : std::string();
        bool explicitDirectory = false;
        if (normaliseEntryPath(item.path, &item.relativePath, &explicitDirectory)) {
            item.name = baseName(item.relativePath);
        } else {
            item.unsafe = true;
            item.name = baseName(item.path);
            if (item.name.empty())
                item.name = item.path;
            ++listing.unsafePaths;
        }

        const mode_t type = archive_entry_filetype(entry);
        item.directory = type == AE_IFDIR || explicitDirectory;
        item.symlink = type == AE_IFLNK;
        item.hardlink = archive_entry_hardlink(entry) != nullptr;
        item.size = item.directory ? 0 : entrySize(entry);
        if (archive_entry_mtime_is_set(entry))
            item.mtime = archive_entry_mtime(entry);
        item.mode = archive_entry_perm(entry);
        if (const char* target = archive_entry_symlink(entry))
            item.linkTarget = target;
        if (archive_entry_is_encrypted(entry) == 1)
            listing.encrypted = true;
        if (!item.directory && !item.symlink && !item.hardlink)
            listing.totalSize += item.size;
        listing.entries.push_back(std::move(item));
    }
    if (const char* formatName = archive_format_name(reader.get()))
        listing.formatName = formatName;
    if (const char* filterName = archive_filter_name(reader.get(), 0))
        listing.filterName = filterName;
    if (archive_read_has_encrypted_entries(reader.get()) > 0)
        listing.encrypted = true;
    listing.ok = true;
    return listing;
}

ExtractResult extractArchive(const std::string& archivePath, const std::vector<Entry>& entries,
                             const std::vector<size_t>& selected, const std::string& destination,
                             const ExtractOptions& options, const ProgressCallback& progress,
                             const std::atomic<bool>* cancel)
{
    ExtractResult result;
    std::string base;
    if (!canonicalDirectory(destination, &base, &result.error))
        return result;
    if (selected.empty()) {
        result.ok = true;
        return result;
    }

    const std::set<size_t> chosen(selected.begin(), selected.end());
    uint64_t total = 0;
    for (const size_t index : chosen) {
        if (index >= entries.size())
            continue;
        const Entry& item = entries[index];
        if (!item.unsafe && !item.directory && !item.symlink && !item.hardlink)
            total += item.size;
    }

    std::string error;
    Reader reader = openReader(archivePath, &error);
    if (!reader) {
        result.error = std::move(error);
        return result;
    }

    struct PendingLink
    {
        std::string relativePath;
        std::string target;
        int64_t mtime;
    };
    struct PendingDirectory
    {
        std::string path;
        int64_t mtime;
    };
    std::vector<PendingLink> links;
    std::vector<PendingDirectory> directories;

    Progress report;
    report.total = total;
    if (progress)
        progress(report);

    struct archive_entry* entry = nullptr;
    size_t index = 0;
    char buffer[64 * 1024];
    while (true) {
        const int status = archive_read_next_header(reader.get(), &entry);
        if (status == ARCHIVE_EOF)
            break;
        if (status < ARCHIVE_WARN) {
            result.error = encryptionMessage(reader.get());
            return result;
        }
        const size_t current = index++;
        if (!chosen.contains(current))
            continue;
        if (isCancelled(cancel)) {
            result.cancelled = true;
            return result;
        }
        if (current >= entries.size()) {
            ++result.skipped;
            continue;
        }
        const Entry& listed = entries[current];

        const char* name = archive_entry_pathname_utf8(entry);
        if (!name)
            name = archive_entry_pathname(entry);
        std::string relative;
        bool explicitDirectory = false;
        const std::string_view raw = name ? std::string_view(name) : std::string_view();
        if (listed.unsafe || !normaliseEntryPath(raw, &relative, &explicitDirectory)) {
            ++result.skipped;
            continue;
        }

        const mode_t type = archive_entry_filetype(entry);
        const bool directory = type == AE_IFDIR || explicitDirectory;
        const mode_t permissions = mode_t(archive_entry_perm(entry) & 0777);
        const int64_t modified = archive_entry_mtime_is_set(entry) ? archive_entry_mtime(entry) : 0;

        if (directory) {
            if (!ensureDirectoryWithin(base, relative, permissions ? permissions : 0755, &result.error))
                return result;
            ++result.directories;
            directories.push_back({joinPath(base, relative), modified});
            continue;
        }

        if (type == AE_IFLNK) {
            const char* target = archive_entry_symlink(entry);
            links.push_back({relative, target ? target : std::string(), modified});
            continue;
        }

        if (type != AE_IFREG || archive_entry_hardlink(entry) != nullptr) {
            // Devices, sockets, FIFOs and hard links are not written out.
            ++result.skipped;
            continue;
        }

        const std::string parent = parentOf(relative);
        if (!parent.empty() && !ensureDirectoryWithin(base, parent, 0755, &result.error))
            return result;
        const std::string finalPath = joinPath(base, relative);
        const int fd = openFileForExtract(finalPath, permissions ? (permissions | S_IRUSR | S_IWUSR) : 0644, options.overwrite);
        if (fd < 0) {
            if (errno == EEXIST) {
                ++result.skipped;
                continue;
            }
            result.error = format(tr("Could not create {path}: {error}"), {{"path", relative}, {"error", std::strerror(errno)}});
            return result;
        }

        report.current = listed.name.empty() ? relative : listed.name;
        bool failed = false;
        while (true) {
            if (isCancelled(cancel)) {
                ::close(fd);
                result.cancelled = true;
                return result;
            }
            const la_ssize_t bytes = archive_read_data(reader.get(), buffer, sizeof(buffer));
            if (bytes < 0) {
                result.error = format(tr("Could not read {path}: {error}"),
                                      {{"path", relative}, {"error", errorText(reader.get())}});
                failed = true;
                break;
            }
            if (bytes == 0)
                break;
            if (!writeAll(fd, buffer, size_t(bytes))) {
                result.error = format(tr("Could not write {path}: {error}"),
                                      {{"path", relative}, {"error", std::strerror(errno)}});
                failed = true;
                break;
            }
            report.completed += uint64_t(bytes);
            result.bytes += uint64_t(bytes);
            if (progress)
                progress(report);
        }
        if (modified > 0) {
            const struct timespec times[2] = {{0, UTIME_OMIT}, {time_t(modified), 0}};
            ::futimens(fd, times);
        }
        if (::close(fd) != 0 && !failed) {
            result.error = format(tr("Could not write {path}: {error}"),
                                  {{"path", relative}, {"error", std::strerror(errno)}});
            failed = true;
        }
        if (failed)
            return result;
        ++result.files;
    }

    // Symbolic links go last: none of them can redirect the files above, and
    // only the ones that stay inside the destination are made.
    for (const PendingLink& link : links) {
        if (isCancelled(cancel)) {
            result.cancelled = true;
            return result;
        }
        if (!options.restoreSymlinks || !linkStaysInside(link.relativePath, link.target)) {
            ++result.skipped;
            continue;
        }
        const std::string parent = parentOf(link.relativePath);
        if (!parent.empty() && !ensureDirectoryWithin(base, parent, 0755, &result.error))
            return result;
        const std::string finalPath = joinPath(base, link.relativePath);
        struct stat st;
        if (::lstat(finalPath.c_str(), &st) == 0 || ::symlink(link.target.c_str(), finalPath.c_str()) != 0) {
            ++result.skipped;
            continue;
        }
        applyTimes(finalPath, link.mtime, false);
        ++result.links;
    }

    // Writing into a folder changes its time, so folders get theirs last.
    for (auto it = directories.rbegin(); it != directories.rend(); ++it)
        applyTimes(it->path, it->mtime, true);

    report.completed = report.total;
    report.current.clear();
    if (progress)
        progress(report);
    result.ok = true;
    return result;
}

CreateResult createArchive(const std::string& archivePath, const std::vector<std::string>& sources,
                           CreateFormat createFormat, const ProgressCallback& progress,
                           const std::atomic<bool>* cancel)
{
    CreateResult result;
    if (sources.empty()) {
        result.error = tr("No files were chosen.");
        return result;
    }

    struct Item
    {
        std::string path;
        std::string entryPath;
        bool directory = false;
        uint64_t size = 0;
        int64_t mtime = 0;
        mode_t mode = 0644;
    };
    std::vector<Item> items;

    for (const std::string& source : sources) {
        struct stat st;
        if (::stat(source.c_str(), &st) != 0) {
            result.error = format(tr("No such file or folder: {path}"), {{"path", source}});
            return result;
        }
        std::string name = fs::path(source).filename().string();
        if (name.empty() || name == "." || name == "..")
            name = fs::path(source).parent_path().filename().string();
        if (name.empty()) {
            result.error = format(tr("The path cannot be archived: {path}"), {{"path", source}});
            return result;
        }
        if (S_ISREG(st.st_mode)) {
            items.push_back({source, name, false, uint64_t(st.st_size), st.st_mtime, mode_t(st.st_mode & 0777)});
        } else if (S_ISDIR(st.st_mode)) {
            items.push_back({source, name + '/', true, 0, st.st_mtime, mode_t(st.st_mode & 0777)});
            std::error_code error;
            fs::recursive_directory_iterator it(source, fs::directory_options::skip_permission_denied, error);
            const fs::recursive_directory_iterator end;
            for (; !error && it != end; it.increment(error)) {
                const fs::path path = it->path();
                std::error_code statusError;
                const fs::file_status status = it->symlink_status(statusError);
                if (statusError)
                    continue;
                std::error_code relativeError;
                const fs::path relative = fs::relative(path, source, relativeError);
                if (relativeError || relative.empty())
                    continue;
                const std::string entryPath = name + '/' + relative.generic_string();
                struct stat entryStatus;
                if (::lstat(path.c_str(), &entryStatus) != 0)
                    continue;
                if (fs::is_directory(status)) {
                    items.push_back({path.string(), entryPath + '/', true, 0, entryStatus.st_mtime,
                                     mode_t(entryStatus.st_mode & 0777)});
                } else if (fs::is_regular_file(status)) {
                    items.push_back({path.string(), entryPath, false, uint64_t(entryStatus.st_size),
                                     entryStatus.st_mtime, mode_t(entryStatus.st_mode & 0777)});
                }
                // Symbolic links, sockets and devices are left out.
            }
            if (error) {
                result.error = format(tr("Could not read {path}: {error}"), {{"path", source}, {"error", error.message()}});
                return result;
            }
        } else {
            result.error = format(tr("{path} is not a regular file or folder."), {{"path", source}});
            return result;
        }
    }

    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.entryPath < b.entryPath; });
    uint64_t total = 0;
    for (const Item& item : items) {
        if (!item.directory)
            total += item.size;
    }

    struct ::archive* writer = archive_write_new();
    if (!writer) {
        result.error = tr("Could not allocate memory for the archive.");
        return result;
    }

    switch (createFormat) {
    case CreateFormat::Zip:
        archive_write_set_format_zip(writer);
        archive_write_set_options(writer, "zip:compression=deflate");
        break;
    case CreateFormat::Tar:
        archive_write_set_format_pax_restricted(writer);
        break;
    case CreateFormat::TarGz:
        archive_write_set_format_pax_restricted(writer);
        archive_write_add_filter_gzip(writer);
        break;
    }

    if (archive_write_open_filename(writer, archivePath.c_str()) != ARCHIVE_OK) {
        result.error = format(tr("Could not create {path}: {error}"),
                              {{"path", archivePath}, {"error", errorText(writer)}});
        archive_write_free(writer);
        return result;
    }

    Progress report;
    report.total = total;
    if (progress)
        progress(report);

    char buffer[64 * 1024];
    for (const Item& item : items) {
        if (isCancelled(cancel)) {
            result.cancelled = true;
            archive_write_close(writer);
            archive_write_free(writer);
            ::unlink(archivePath.c_str());
            return result;
        }
        struct archive_entry* entry = archive_entry_new();
        if (!entry) {
            result.error = tr("Could not allocate memory for an archive entry.");
            break;
        }
        archive_entry_set_pathname(entry, item.entryPath.c_str());
        archive_entry_set_perm(entry, item.mode ? item.mode : (item.directory ? 0755 : 0644));
        if (item.mtime > 0)
            archive_entry_set_mtime(entry, time_t(item.mtime), 0);
        if (item.directory) {
            archive_entry_set_filetype(entry, AE_IFDIR);
            archive_entry_set_size(entry, 0);
        } else {
            struct stat st;
            if (::stat(item.path.c_str(), &st) != 0) {
                archive_entry_free(entry);
                continue;
            }
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_size(entry, st.st_size);
            archive_entry_set_perm(entry, mode_t(st.st_mode & 0777));
        }
        if (archive_write_header(writer, entry) != ARCHIVE_OK) {
            result.error = format(tr("Could not add {path}: {error}"),
                                  {{"path", item.entryPath}, {"error", errorText(writer)}});
            archive_entry_free(entry);
            break;
        }
        if (item.directory) {
            archive_entry_free(entry);
            ++result.directories;
            continue;
        }

        const int fd = ::open(item.path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            result.error = format(tr("Could not read {path}: {error}"),
                                  {{"path", item.path}, {"error", std::strerror(errno)}});
            archive_entry_free(entry);
            break;
        }
        bool failed = false;
        while (true) {
            if (isCancelled(cancel)) {
                ::close(fd);
                archive_entry_free(entry);
                result.cancelled = true;
                archive_write_close(writer);
                archive_write_free(writer);
                ::unlink(archivePath.c_str());
                return result;
            }
            const ssize_t bytes = ::read(fd, buffer, sizeof(buffer));
            if (bytes < 0) {
                result.error = format(tr("Could not read {path}: {error}"),
                                      {{"path", item.path}, {"error", std::strerror(errno)}});
                failed = true;
                break;
            }
            if (bytes == 0)
                break;
            if (archive_write_data(writer, buffer, size_t(bytes)) < 0) {
                result.error = format(tr("Could not write {path}: {error}"),
                                      {{"path", item.entryPath}, {"error", errorText(writer)}});
                failed = true;
                break;
            }
            report.completed += uint64_t(bytes);
            result.bytes += uint64_t(bytes);
            if (progress)
                progress(report);
        }
        ::close(fd);
        archive_entry_free(entry);
        if (failed)
            break;
        ++result.files;
    }

    if (!result.error.empty() || result.cancelled) {
        archive_write_close(writer);
        archive_write_free(writer);
        ::unlink(archivePath.c_str());
        return result;
    }
    if (archive_write_close(writer) != ARCHIVE_OK) {
        result.error = format(tr("Could not finish {path}: {error}"),
                              {{"path", archivePath}, {"error", errorText(writer)}});
        archive_write_free(writer);
        ::unlink(archivePath.c_str());
        return result;
    }
    archive_write_free(writer);
    report.completed = report.total;
    report.current.clear();
    if (progress)
        progress(report);
    result.ok = true;
    return result;
}

std::string formatLabel(const std::string& archivePath, const std::string& libarchiveFormat,
                        const std::string& filterName)
{
    const std::string name = lowerExtension(archivePath);
    if (endsWith(name, ".tar.gz") || endsWith(name, ".tgz"))
        return "TAR.GZ";
    if (endsWith(name, ".tar.bz2") || endsWith(name, ".tbz2") || endsWith(name, ".tbz"))
        return "TAR.BZ2";
    if (endsWith(name, ".tar.xz") || endsWith(name, ".txz"))
        return "TAR.XZ";
    if (endsWith(name, ".tar.zst") || endsWith(name, ".tzst"))
        return "TAR.ZST";
    if (endsWith(name, ".tar.lzma") || endsWith(name, ".tlz"))
        return "TAR.LZMA";
    if (endsWith(name, ".tar"))
        return "TAR";
    if (endsWith(name, ".zip") || endsWith(name, ".jar") || endsWith(name, ".cbz") || endsWith(name, ".epub")
        || endsWith(name, ".apk"))
        return "ZIP";
    if (endsWith(name, ".7z"))
        return "7Z";
    if (endsWith(name, ".rar"))
        return "RAR";
    if (endsWith(name, ".cpio"))
        return "CPIO";
    if (endsWith(name, ".iso"))
        return "ISO";
    if (endsWith(name, ".deb"))
        return "DEB";
    if (endsWith(name, ".rpm"))
        return "RPM";
    if (endsWith(name, ".gz"))
        return "GZ";
    if (endsWith(name, ".bz2"))
        return "BZ2";
    if (endsWith(name, ".xz"))
        return "XZ";
    if (endsWith(name, ".zst"))
        return "ZST";

    const std::string formatName = upperCopy(libarchiveFormat);
    const auto has = [&formatName](std::string_view text) { return formatName.find(text) != std::string::npos; };
    if (has("7-ZIP") || has("7ZIP"))
        return "7Z";
    if (has("RAR"))
        return "RAR";
    if (has("ZIP"))
        return "ZIP";
    if (has("USTAR") || has("TAR"))
        return "TAR";
    if (has("CPIO"))
        return "CPIO";
    if (has("ISO"))
        return "ISO";
    if (has("MTREE"))
        return "MTREE";
    if (!filterName.empty() && filterName != "none")
        return upperCopy(filterName);
    return "ARCHIVE";
}

bool createFormatForPath(const std::string& path, CreateFormat* format)
{
    const std::string name = lowerExtension(path);
    if (endsWith(name, ".zip")) {
        *format = CreateFormat::Zip;
        return true;
    }
    if (endsWith(name, ".tar")) {
        *format = CreateFormat::Tar;
        return true;
    }
    if (endsWith(name, ".tar.gz") || endsWith(name, ".tgz")) {
        *format = CreateFormat::TarGz;
        return true;
    }
    return false;
}

std::string archiveNameFilter()
{
    return "*.zip *.tar *.tar.gz *.tgz *.tar.bz2 *.tbz2 *.tar.xz *.txz *.tar.zst *.tzst *.7z *.rar *.cpio *.iso "
           "*.deb *.jar";
}

std::string archiveMimeTypes()
{
    return "application/zip;application/x-tar;application/gzip;application/x-7z-compressed;application/vnd.rar;"
           "application/x-rar;application/x-compressed-tar;application/x-bzip-compressed-tar;"
           "application/x-xz-compressed-tar;application/zstd;application/x-cpio;application/x-iso9660-image;"
           "application/vnd.debian.binary-package";
}

} // namespace archivemanager
