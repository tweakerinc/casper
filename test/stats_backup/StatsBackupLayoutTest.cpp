#include <gtest/gtest.h>

#include "StatsBackupLayout.h"

TEST(StatsBackupLayout, TrashDirIsDotTrashUnderCache) {
  char path[128];
  ASSERT_TRUE(statsbackup::trashDirPath("/.crosspoint/epub_42", path, sizeof(path)));
  EXPECT_STREQ(path, "/.crosspoint/epub_42/.trash");
  EXPECT_FALSE(statsbackup::trashDirPath("", path, sizeof(path)));
  EXPECT_FALSE(statsbackup::trashDirPath(nullptr, path, sizeof(path)));
}

TEST(StatsBackupLayout, TrashFileKeepsLiveName) {
  char path[160];
  ASSERT_TRUE(statsbackup::trashFilePath("/.crosspoint/epub_42", "stats_v6.bin", path, sizeof(path)));
  EXPECT_STREQ(path, "/.crosspoint/epub_42/.trash/stats_v6.bin");
  EXPECT_FALSE(statsbackup::trashFilePath("/.crosspoint/epub_42", "", path, sizeof(path)));
  EXPECT_FALSE(statsbackup::trashFilePath(nullptr, "stats_v6.bin", path, sizeof(path)));
}

TEST(StatsBackupLayout, GlobalBackupDirUnchanged) {
  EXPECT_STREQ(statsbackup::kDir, "/.crosspoint-stats-backup");
  EXPECT_STREQ(statsbackup::kLegacyCasperDir, "/.casper-stats-backup");
  EXPECT_STREQ(statsbackup::kTrashFolder, ".trash");
}
