cmake_minimum_required(VERSION 3.21)

set(PROJECT_NAME GDPracticeMapper)
project(${PROJECT_NAME} VERSION 1.0.1)

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "GEODE_SDK is not set. Run `geode sdk install` first.")
endif()

set(GEODE_DISABLE_PRECOMPILED_HEADERS ON)

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

add_library(${PROJECT_NAME} SHARED
    src/main.cpp
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
set(GEODE_LINK_SOURCE ON)
set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/mod.json.in ${CMAKE_CURRENT_SOURCE_DIR}/mod.json @ONLY)
setup_geode_mod(${PROJECT_NAME})
