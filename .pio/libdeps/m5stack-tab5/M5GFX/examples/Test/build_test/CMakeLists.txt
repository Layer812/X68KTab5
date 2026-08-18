cmake_minimum_required(VERSION 3.16.0)
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../..")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(M5GFX-build-test)
