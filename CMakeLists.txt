# SPDX-FileCopyrightText: 2026 quendoris
# SPDX-License-Identifier: AGPL-3.0-only

cmake_minimum_required(VERSION 3.20)
project(aeris_desktop VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets)

set(AERIS_CORE_SOURCE_DIR "" CACHE PATH "Path to a local aeris-core checkout")
if(NOT AERIS_CORE_SOURCE_DIR)
    set(_aeris_sibling "${CMAKE_CURRENT_SOURCE_DIR}/../aeris-core")
    if(EXISTS "${_aeris_sibling}/CMakeLists.txt")
        set(AERIS_CORE_SOURCE_DIR "${_aeris_sibling}")
    else()
        message(FATAL_ERROR
            "AERIS core checkout not found. Clone quendoris/aeris-core next to this repository "
            "or configure -DAERIS_CORE_SOURCE_DIR=/path/to/aeris-core"
        )
    endif()
endif()

set(AERIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(AERIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(AERIS_BUILD_VIEWER OFF CACHE BOOL "" FORCE)
set(AERIS_BUILD_STORAGE ON CACHE BOOL "" FORCE)
set(AERIS_BUILD_PROJECT ON CACHE BOOL "" FORCE)
add_subdirectory("${AERIS_CORE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/aeris-core")

add_executable(aeris-desktop
    src/main.cpp
    src/main_window.cpp
    src/main_window.hpp
    src/map_view.cpp
    src/map_view.hpp
    src/map_view_projection.cpp
    src/map_workspace_view.cpp
    src/map_workspace_view.hpp
    src/project_model.cpp
    src/project_model.hpp
    src/scene_controller.cpp
    src/scene_controller.hpp
)

target_link_libraries(aeris-desktop PRIVATE
    AERIS::core
    AERIS::storage
    AERIS::project
    Qt6::Core
    Qt6::Widgets
)
target_compile_features(aeris-desktop PRIVATE cxx_std_17)
set_target_properties(aeris-desktop PROPERTIES
    CXX_EXTENSIONS OFF
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)

add_executable(aeris-demo-project dev/make_demo_project.cpp)
target_link_libraries(aeris-demo-project PRIVATE
    AERIS::core
    AERIS::storage
    AERIS::project
)
target_compile_features(aeris-demo-project PRIVATE cxx_std_17)
set_target_properties(aeris-demo-project PROPERTIES CXX_EXTENSIONS OFF)

add_executable(aeris-desktop-render-probe
    dev/render_project.cpp
    src/map_view.cpp
    src/map_view.hpp
    src/map_view_projection.cpp
    src/map_workspace_view.cpp
    src/map_workspace_view.hpp
    src/project_model.cpp
    src/project_model.hpp
)
target_include_directories(aeris-desktop-render-probe PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(aeris-desktop-render-probe PRIVATE
    AERIS::core
    AERIS::storage
    AERIS::project
    Qt6::Core
    Qt6::Widgets
)
target_compile_features(aeris-desktop-render-probe PRIVATE cxx_std_17)
set_target_properties(aeris-desktop-render-probe PROPERTIES CXX_EXTENSIONS OFF)

add_executable(aeris-desktop-projection-catalog-probe
    dev/projection_catalog_probe.cpp
    src/map_view.cpp
    src/map_view.hpp
    src/map_view_projection.cpp
    src/map_workspace_view.cpp
    src/map_workspace_view.hpp
    src/project_model.cpp
    src/project_model.hpp
)
target_include_directories(aeris-desktop-projection-catalog-probe PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(aeris-desktop-projection-catalog-probe PRIVATE
    AERIS::core
    AERIS::storage
    AERIS::project
    Qt6::Core
    Qt6::Widgets
)
target_compile_features(aeris-desktop-projection-catalog-probe PRIVATE cxx_std_17)
set_target_properties(aeris-desktop-projection-catalog-probe PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
    target_compile_options(aeris-desktop PRIVATE /W4 /permissive-)
    target_compile_options(aeris-demo-project PRIVATE /W4 /permissive-)
    target_compile_options(aeris-desktop-render-probe PRIVATE /W4 /permissive-)
    target_compile_options(aeris-desktop-projection-catalog-probe PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeris-desktop PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    target_compile_options(aeris-demo-project PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    target_compile_options(aeris-desktop-render-probe PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    target_compile_options(aeris-desktop-projection-catalog-probe PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()
