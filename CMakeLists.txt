cmake_minimum_required(VERSION 3.16)
project(motion_blur_plugin CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(OpenMP)
add_library(mbcore STATIC core/motion_blur.cpp)
target_include_directories(mbcore PUBLIC core)
set_target_properties(mbcore PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(OpenMP_CXX_FOUND)
  target_link_libraries(mbcore PUBLIC OpenMP::OpenMP_CXX)
endif()

enable_testing()
add_executable(test_motion_blur tests/test_motion_blur.cpp)
target_link_libraries(test_motion_blur mbcore)
add_test(NAME motion_blur COMMAND test_motion_blur)

# After Effects plugin: configure with -DAE_SDK_DIR=/path/to/AfterEffectsSDK
if(AE_SDK_DIR)
  add_library(VectorMotionBlur MODULE ae/MotionBlur.cpp
              "${AE_SDK_DIR}/Examples/Util/AEGP_SuiteHandler.cpp")
  target_include_directories(VectorMotionBlur PRIVATE
    "${AE_SDK_DIR}/Examples/Headers" "${AE_SDK_DIR}/Examples/Headers/SP"
    "${AE_SDK_DIR}/Examples/Resources" "${AE_SDK_DIR}/Examples/Util")
  target_link_libraries(VectorMotionBlur PRIVATE mbcore)
  if(WIN32)
    set_target_properties(VectorMotionBlur PROPERTIES SUFFIX ".aex" PREFIX "")
    # PiPL: compile ae/MotionBlur.r with Adobe's PiPLtool + cl /EP, see README.
  elseif(APPLE)
    set_target_properties(VectorMotionBlur PROPERTIES BUNDLE TRUE BUNDLE_EXTENSION "plugin")
  endif()
endif()
