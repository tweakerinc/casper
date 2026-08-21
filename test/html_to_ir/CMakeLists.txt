add_executable(HtmlToIrTest
  HtmlToIrTest.cpp
  ${REPO_ROOT}/lib/Rivulet/HtmlToIr.cpp
  ${REPO_ROOT}/lib/Rivulet/ChapterIr.cpp
  ${REPO_ROOT}/lib/Rivulet/IrTokenizer.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

# stubs/ comes FIRST so <Esp.h>, <Logging.h> and <HalStorage.h> resolve to the
# host shims instead of the firmware headers (which need Arduino/SdFat).
# Serialization.h is the real one — it only needs HalFile's read/write shape.
target_include_directories(HtmlToIrTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Rivulet
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Serialization
)

target_link_libraries(HtmlToIrTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HtmlToIrTest)
