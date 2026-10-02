add_executable(MusicNoteFallbackTest
  MusicNoteFallbackTest.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/MusicNoteFallback.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(MusicNoteFallbackTest PRIVATE
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(MusicNoteFallbackTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(MusicNoteFallbackTest)
