add_executable(FutureChapterIndexTest
  FutureChapterIndexTest.cpp
)

target_include_directories(FutureChapterIndexTest PRIVATE
  ${REPO_ROOT}/src/activities/reader
)

target_link_libraries(FutureChapterIndexTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FutureChapterIndexTest)
