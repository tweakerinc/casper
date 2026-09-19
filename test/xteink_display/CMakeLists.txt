# Host tests for the pinned freeink-sdk X3 UC8279d probe + driver.
# These compile the SDK's own tests so a stale submodule pin fails CI.

set(SDK "${REPO_ROOT}/freeink-sdk")
set(DETECT "${SDK}/libs/hardware/XteinkDetect")
set(DISPLAY "${SDK}/libs/display/FreeInkDisplay")

add_executable(XteinkDisplayProbeTest
  ${DETECT}/test/host/test_display_probe.cpp
  ${DETECT}/src/XteinkDetect.cpp
)
target_include_directories(XteinkDisplayProbeTest PRIVATE
  ${DETECT}/test/host/stubs
  ${SDK}/libs/hardware/BoardConfig/include
  ${DETECT}/include
)
target_compile_definitions(XteinkDisplayProbeTest PRIVATE
  FREEINK_DEVICE_X3=1
  FREEINK_DEVICE_X4=1
)
# Protocol asserts must run even when the rest of the host tests are Release.
target_compile_options(XteinkDisplayProbeTest PRIVATE
  -Wall
  -Wextra
  -UNDEBUG
)
add_test(NAME XteinkDisplayProbeTest COMMAND XteinkDisplayProbeTest)

add_test(NAME Uc8279DriverHostTest
  COMMAND ${Python3_EXECUTABLE} ${DISPLAY}/test/host/run_uc8279.py
)
set_tests_properties(Uc8279DriverHostTest PROPERTIES
  WORKING_DIRECTORY ${DISPLAY}/test/host
  ENVIRONMENT "CXX=${CMAKE_CXX_COMPILER}"
)

add_test(NAME Uc8253PowerHostTest
  COMMAND ${Python3_EXECUTABLE} ${DISPLAY}/test/host/run_uc8253_power.py
)
set_tests_properties(Uc8253PowerHostTest PROPERTIES
  WORKING_DIRECTORY ${DISPLAY}/test/host
  ENVIRONMENT "CXX=${CMAKE_CXX_COMPILER}"
)
