// SPDX-FileCopyrightText: 2026 LingmoOS Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "archiveentry.h"

#include <gtest/gtest.h>

#include <filesystem>
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
