add_executable(StatsBackupLayoutTest
  StatsBackupLayoutTest.cpp
)

target_include_directories(StatsBackupLayoutTest PRIVATE
  ${REPO_ROOT}/src/activities/reader
)

target_link_libraries(StatsBackupLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(StatsBackupLayoutTest)
