# SPDX-FileCopyrightText: 2026 quendoris
# SPDX-License-Identifier: AGPL-3.0-only

cmake_minimum_required(VERSION 3.20)
project(aeris_desktop VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent Network)
find_package(TIFF REQUIRED)

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
    src/data_job_process.cpp
    src/data_job_process.hpp
    src/data_job_progress.hpp
    src/elevation_detail_loader.cpp
    src/elevation_detail_loader.hpp
    src/elevation_import.cpp
    src/elevation_import.hpp
    src/elevation_renderer.cpp
    src/elevation_renderer.hpp
    src/elevation_tiff.cpp
    src/elevation_tiff.hpp
    src/elevation_tiff_inspect.cpp
    src/elevation_ui.cpp
    src/elevation_ui.hpp
    src/flag_pack_import.cpp
    src/flag_pack_import.hpp
    src/flag_renderer.cpp
    src/flag_renderer.hpp
    src/flag_ui.cpp
    src/flag_ui.hpp
    src/main.cpp
    src/main_window.cpp
    src/main_window_elevation.cpp
    src/main_window_flags.cpp
    src/main_window_shutdown.cpp
    src/main_window_startup.cpp
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
    src/world_data_import.cpp
    src/world_data_import.hpp
)

target_link_libraries(aeris-desktop PRIVATE
    AERIS::core
    AERIS::storage
    AERIS::project
    TIFF::TIFF
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Concurrent
)
target_compile_features(aeris-desktop PRIVATE cxx_std_17)
set_target_properties(aeris-desktop PROPERTIES
    CXX_EXTENSIONS OFF
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)

# Long acquisition/import work belongs outside the GUI process. Network access
# is intentionally confined to this worker; the renderer/core remain network-
# free. Exact immutable URLs and SHA-256 verification are applied before source
# adapters see acquired bytes.
add_executable(aeris-data-worker
    src/data_job_progress.hpp
    src/data_worker_main.cpp
    src/elevation_import.cpp
    src/elevation_import.hpp
    src/elevation_tiff.cpp
    src/elevation_tiff.hpp
    src/elevation_tiff_inspect.cpp
    src/flag_pack_import.cpp
    src/flag_pack_import.hpp
    src/natural_earth_acquisition.cpp
    src/natural_earth_acquisition.hpp
    src/world_data_import.cpp
    src/world_data_import.hpp
)
target_include_directories(aeris-data-worker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(aeris-data-worker PRIVATE
    AERIS::core
    AERIS::storage
    AERIS::project
    TIFF::TIFF
    Qt6::Core
    Qt6::Network
)
target_compile_features(aeris-data-worker PRIVATE cxx_std_17)
set_target_properties(aeris-data-worker PROPERTIES CXX_EXTENSIONS OFF)

add_dependencies(aeris-desktop aeris-data-worker)
