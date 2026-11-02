# SPDX-FileCopyrightText: 2026 LingmoOS Contributors
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.16)

project(lingmo-archive-manager VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)
include(CTest)

find_package(LingmoSDK REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBARCHIVE REQUIRED IMPORTED_TARGET libarchive)

add_library(archive-core STATIC
    src/archivebackend.cpp
)
target_include_directories(archive-core PUBLIC src)
target_link_libraries(archive-core PUBLIC Lingmo::SDKBase PkgConfig::LIBARCHIVE)

add_executable(lingmo-archive-manager src/main.cpp)
target_link_libraries(lingmo-archive-manager PRIVATE archive-core Lingmo::SDK)
target_compile_definitions(lingmo-archive-manager PRIVATE
    ARCHIVE_MANAGER_VERSION="${PROJECT_VERSION}")

install(TARGETS lingmo-archive-manager RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
lingmo_add_translations(lingmo-archive-manager SOURCES src
    DESKTOP_FILES lingmo-archive-manager.desktop)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/lingmo-archive-manager.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
