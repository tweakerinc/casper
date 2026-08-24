add_executable(FontLadderTest
  FontLadderTest.cpp
  ${REPO_ROOT}/lib/Rivulet/FontLadder.cpp
)

target_include_directories(FontLadderTest PRIVATE
  ${REPO_ROOT}/lib/Rivulet
  ${REPO_ROOT}/lib/EpdFont
)

target_link_libraries(FontLadderTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FontLadderTest)
