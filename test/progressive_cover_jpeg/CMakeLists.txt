add_executable(ProgressiveCoverJpegTest
  ProgressiveCoverJpegTest.cpp
  ${REPO_ROOT}/lib/jpgd/jpgd.cpp
  ${REPO_ROOT}/lib/jpgd/jpgd_spill.cpp
)

target_include_directories(ProgressiveCoverJpegTest PRIVATE
  ${REPO_ROOT}/lib/jpgd
)

target_compile_definitions(ProgressiveCoverJpegTest PRIVATE
  JPGD_USE_SSE2=0
)

target_link_libraries(ProgressiveCoverJpegTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ProgressiveCoverJpegTest
  WORKING_DIRECTORY ${REPO_ROOT}
)
