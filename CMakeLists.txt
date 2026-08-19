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

function(aeris_desktop_target target_name)
    target_include_directories(${target_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(${target_name} PRIVATE cxx_std_17)
    set_target_properties(${target_name} PROPERTIES CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4 /permissive-)
        if(AERIS_DESKTOP_WARNINGS_AS_ERRORS)
            target_compile_options(${target_name} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
        if(AERIS_DESKTOP_WARNINGS_AS_ERRORS)
            target_compile_options(${target_name} PRIVATE -Werror)
        endif()
    endif()
endfunction()

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
target_link_libraries(aeris_viewer PRIVATE AERIS::core Qt6::Widgets)
aeris_desktop_target(aeris_viewer)
set_target_properties(aeris_viewer PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)

add_executable(aeris_viewer_scene_probe
    tests/scene_probe.cpp
    src/scene_builder.cpp
    src/scene_presentation.cpp
    src/unfold.cpp
    src/world_loader.cpp
)
target_link_libraries(aeris_viewer_scene_probe PRIVATE AERIS::core)
aeris_desktop_target(aeris_viewer_scene_probe)

add_executable(aeris_viewer_controller_probe
    tests/controller_probe.cpp
    src/scene_builder.cpp
    src/scene_controller.cpp
    src/scene_presentation.cpp
    src/unfold.cpp
    src/unfold_controller.cpp
    src/world_loader.cpp
)
target_link_libraries(aeris_viewer_controller_probe PRIVATE AERIS::core Qt6::Core)
aeris_desktop_target(aeris_viewer_controller_probe)

add_executable(aeris_viewer_layer_stack_probe
    tests/layer_stack_probe.cpp
    src/layer_stack.cpp
)
target_link_libraries(aeris_viewer_layer_stack_probe PRIVATE AERIS::core)
aeris_desktop_target(aeris_viewer_layer_stack_probe)
