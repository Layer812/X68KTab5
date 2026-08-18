cmake_minimum_required(VERSION 3.16.0)

# Pull in M5Unified itself (this repository's root, three levels up).
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../..")

# Pull in M5GFX. CI exports M5GFX_PATH; for local use, fall back to a
# sibling checkout (../../M5GFX next to the M5Unified repo).
if(DEFINED ENV{M5GFX_PATH})
  list(APPEND EXTRA_COMPONENT_DIRS "$ENV{M5GFX_PATH}")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../../../M5GFX/CMakeLists.txt")
  list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../../M5GFX")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(M5Unified-build-test)
