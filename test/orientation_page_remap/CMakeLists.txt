add_executable(OrientationPageRemapTest
  OrientationPageRemapTest.cpp
)

# PageMap now contains an SD-backed record window. Cursor-only tests still
# need the host HAL header even though they do not instantiate disk storage.
target_include_directories(OrientationPageRemapTest PRIVATE
  ${REPO_ROOT}/test/html_to_ir/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Rivulet
)

target_link_libraries(OrientationPageRemapTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OrientationPageRemapTest)
