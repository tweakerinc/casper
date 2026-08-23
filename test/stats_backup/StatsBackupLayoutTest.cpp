#include <gtest/gtest.h>

#include "StatsBackupLayout.h"

TEST(StatsBackupLayout, AcceptsProductCacheFolderNames) {
  EXPECT_TRUE(statsbackup::isBookCacheFolderName("epub_123456"));
  EXPECT_TRUE(statsbackup::isBookCacheFolderName("xtc_1"));
  EXPECT_TRUE(statsbackup::isBookCacheFolderName("txt_99"));
  EXPECT_TRUE(statsbackup::isBookCacheFolderName("book_ab"));
  EXPECT_FALSE(statsbackup::isBookCacheFolderName("rivulet"));
  EXPECT_FALSE(statsbackup::isBookCacheFolderName("global_stats.bin"));
  EXPECT_FALSE(statsbackup::isBookCacheFolderName(""));
  EXPECT_FALSE(statsbackup::isBookCacheFolderName(nullptr));
}

TEST(StatsBackupLayout, BookBackupFileNameIsFolderPlusBin) {
  char name[64];
  ASSERT_TRUE(statsbackup::bookBackupFileName("epub_42", name, sizeof(name)));
  EXPECT_STREQ(name, "epub_42.bin");
  EXPECT_FALSE(statsbackup::bookBackupFileName("recent.json", name, sizeof(name)));
}

TEST(StatsBackupLayout, SnapIdFromDatedGlobalBackup) {
  char id[32];
  ASSERT_TRUE(statsbackup::snapIdFromGlobalFileName("stats_2026-08-23.bin", id, sizeof(id)));
  EXPECT_STREQ(id, "2026-08-23");
  ASSERT_TRUE(statsbackup::snapIdFromGlobalFileName("stats_2026-08-23_1430.bin", id, sizeof(id)));
  EXPECT_STREQ(id, "2026-08-23");
}

TEST(StatsBackupLayout, SnapIdFromIncrementingGlobalBackup) {
  char id[32];
  ASSERT_TRUE(statsbackup::snapIdFromGlobalFileName("stats_backup_003.bin", id, sizeof(id)));
  EXPECT_STREQ(id, "backup_003");
}

TEST(StatsBackupLayout, SnapIdRejectsJunk) {
  char id[32];
  EXPECT_FALSE(statsbackup::snapIdFromGlobalFileName("notes.bin", id, sizeof(id)));
  EXPECT_FALSE(statsbackup::snapIdFromGlobalFileName("stats_.bin", id, sizeof(id)));
  EXPECT_FALSE(statsbackup::snapIdFromGlobalFileName(nullptr, id, sizeof(id)));
}
