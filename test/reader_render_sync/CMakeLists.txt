add_executable(ReaderRenderSyncTest
  ReaderRenderSyncTest.cpp
)

target_include_directories(ReaderRenderSyncTest PRIVATE
  ${REPO_ROOT}/src/activities/reader
)

target_link_libraries(ReaderRenderSyncTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReaderRenderSyncTest)
