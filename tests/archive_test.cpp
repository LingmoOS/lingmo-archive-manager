// SPDX-FileCopyrightText: 2026 LingmoOS Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "archiveentry.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#ifndef ARCHIVE_SOURCE_DIR
#define ARCHIVE_SOURCE_DIR "."
#endif

TEST(ArchiveListing, ReadsSampleZip) {
    const std::string path = std::string(ARCHIVE_SOURCE_DIR) + "/tests/data/sample.zip";
    const archivemanager::Listing listing = archivemanager::listArchive(path);
    ASSERT_TRUE(listing.ok) << listing.error;
    ASSERT_EQ(listing.entries.size(), 3u);
    EXPECT_EQ(listing.entries.front().relativePath, "readme.txt");
    EXPECT_EQ(listing.entries.back().relativePath, "documents/notes/hello.txt");
    EXPECT_GT(listing.totalSize, 0u);
}

TEST(ArchiveCreate, WritesAZipThatCanBeListedAgain) {
    const std::filesystem::path root =
            std::filesystem::temp_directory_path() / "lingmo-archive-manager-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "folder");
    {
        std::ofstream file(root / "folder/hello.txt");
        file << "hello from a created archive\n";
    }
    const std::filesystem::path archivePath = root / "created.zip";
    const archivemanager::CreateResult created = archivemanager::createArchive(
            archivePath.string(), {(root / "folder").string()}, archivemanager::CreateFormat::Zip);
    ASSERT_TRUE(created.ok) << created.error;
    ASSERT_TRUE(std::filesystem::exists(archivePath));
    EXPECT_EQ(created.files, 1u);

    const archivemanager::Listing listing = archivemanager::listArchive(archivePath.string());
    ASSERT_TRUE(listing.ok) << listing.error;
    ASSERT_GE(listing.entries.size(), 1u);
    bool sawFile = false;
    for (const archivemanager::Entry &entry : listing.entries) {
        if (entry.relativePath == "folder/hello.txt")
            sawFile = true;
    }
    EXPECT_TRUE(sawFile);
    std::filesystem::remove_all(root);
}

TEST(ArchiveExtract, ExtractsTheSampleIntoAFolder)
{
    const std::string archivePath = std::string(ARCHIVE_SOURCE_DIR) + "/tests/data/sample.zip";
    const archivemanager::Listing listing = archivemanager::listArchive(archivePath);
    ASSERT_TRUE(listing.ok) << listing.error;
    const std::filesystem::path destination =
            std::filesystem::temp_directory_path() / "lingmo-archive-manager-extract";
    std::filesystem::remove_all(destination);
    std::filesystem::create_directories(destination);
    std::vector<size_t> selected(listing.entries.size());
    for (size_t i = 0; i < selected.size(); ++i)
        selected[i] = i;
    const archivemanager::ExtractResult result = archivemanager::extractArchive(
            archivePath, listing.entries, selected, destination.string(),
            archivemanager::ExtractOptions {});
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.files, 3u);
    EXPECT_TRUE(std::filesystem::exists(destination / "readme.txt"));
    EXPECT_TRUE(std::filesystem::exists(destination / "documents/notes/hello.txt"));
    std::filesystem::remove_all(destination);
}
