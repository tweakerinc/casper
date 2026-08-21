add_executable(SerializedStringBoundTest
  SerializedStringBoundTest.cpp
)

target_include_directories(SerializedStringBoundTest PRIVATE
  ${REPO_ROOT}/lib/Serialization
)

target_link_libraries(SerializedStringBoundTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SerializedStringBoundTest)
