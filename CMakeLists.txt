# SPDX-FileCopyrightText: 2026 quendoris
# SPDX-License-Identifier: AGPL-3.0-only

cmake_minimum_required(VERSION 3.16)
project(aeris-desktop VERSION 0.1.0 DESCRIPTION "AERIS desktop cartographic workbench" LANGUAGES CXX)

include(FetchContent)

option(AERIS_DESKTOP_WARNINGS_AS_ERRORS "Treat desktop warnings as errors" OFF)
set(
    AERIS_CORE_GIT_REPOSITORY
    "https://github.com/quendoris/cartography-aeris.git"
    CACHE STRING
    "Git repository containing the normative AERIS core"
)
set(
    AERIS_CORE_GIT_TAG
    "6e79d1e9ca52e99d7a1e12580386a30d9104a15d"
    CACHE STRING
    "Exact AERIS core commit consumed by this desktop build"
)

# The desktop owns the Qt application. Pull only the platform-neutral core from
# the format repository; never acquire its historical in-tree viewer target.
set(AERIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(AERIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(AERIS_BUILD_STORAGE OFF CACHE BOOL "" FORCE)
set(AERIS_BUILD_PROJECT OFF CACHE BOOL "" FORCE)
set(AERIS_BUILD_VIEWER OFF CACHE BOOL "" FORCE)
set(AERIS_WARNINGS_AS_ERRORS ${AERIS_DESKTOP_WARNINGS_AS_ERRORS} CACHE BOOL "" FORCE)

FetchContent_Declare(
    aeris_core
    GIT_REPOSITORY ${AERIS_CORE_GIT_REPOSITORY}
    GIT_TAG ${AERIS_CORE_GIT_TAG}
    GIT_SHALLOW FALSE
)
FetchContent_MakeAvailable(aeris_core)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets)

add_executable(aeris_viewer
    src/main.cpp
    src/main_window.cpp
    src/layer_stack.cpp
    src/map_canvas.cpp
    src/render_surface.cpp
    src/scene_builder.cpp
    src/scene_controller.cpp
    src/scene_presentation.cpp
    src/unfold.cpp
    src/unfold_controller.cpp
    src/world_loader.cpp
)

target_include_directories(aeris_viewer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(aeris_viewer PRIVATE AERIS::core Qt6::Widgets)
target_compile_features(aeris_viewer PRIVATE cxx_std_17)
set_target_properties(aeris_viewer PROPERTIES
    CXX_EXTENSIONS OFF
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)

if(MSVC)
    target_compile_options(aeris_viewer PRIVATE /W4 /permissive-)
    if(AERIS_DESKTOP_WARNINGS_AS_ERRORS)
        target_compile_options(aeris_viewer PRIVATE /WX)
    endif()
else()
    target_compile_options(aeris_viewer PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    if(AERIS_DESKTOP_WARNINGS_AS_ERRORS)
        target_compile_options(aeris_viewer PRIVATE -Werror)
    endif()
endif()
