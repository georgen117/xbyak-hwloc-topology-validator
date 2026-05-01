cmake_minimum_required(VERSION 3.16)
project(cpu_topology_test VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD   11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(_HWLOC_SUB "${CMAKE_CURRENT_SOURCE_DIR}/extern/hwloc")

# ─── hwloc – always built as a static library from the git submodule ─────────
#
# Windows  : uses extern/hwloc/contrib/windows-cmake (pure CMake, no autotools)
#            with BUILD_SHARED_LIBS=OFF forced so we get a static .lib.
#
# Linux    : uses autotools (configure + make) via ExternalProject_Add with
#            --enable-static --disable-shared.
#            Requires: autoconf automake libtool  (apt install / dnf install)
# ─────────────────────────────────────────────────────────────────────────────

if(NOT EXISTS "${_HWLOC_SUB}/include/hwloc.h")
    message(FATAL_ERROR
        "hwloc submodule is not initialised (${_HWLOC_SUB}/include/hwloc.h missing).\n"
        "Run:  git submodule update --init --recursive")
endif()

if(WIN32)
    # ── Windows: contrib/windows-cmake ───────────────────────────────────────
    set(_HWLOC_CMAKE_DIR "${_HWLOC_SUB}/contrib/windows-cmake")
    if(NOT EXISTS "${_HWLOC_CMAKE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "Expected hwloc cmake build at ${_HWLOC_CMAKE_DIR}.\n"
            "Ensure the submodule is up to date.")
    endif()
    message(STATUS "hwloc: static build from submodule (windows-cmake)")

    # Force static library – must be set before add_subdirectory
    set(BUILD_SHARED_LIBS    OFF CACHE BOOL "" FORCE)
    set(HWLOC_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(HWLOC_SKIP_LSTOPO    ON  CACHE BOOL "" FORCE)
    set(HWLOC_SKIP_TOOLS     ON  CACHE BOOL "" FORCE)
    set(HWLOC_WITH_OPENCL    OFF CACHE BOOL "" FORCE)
    set(HWLOC_WITH_CUDA      OFF CACHE BOOL "" FORCE)

    add_subdirectory("${_HWLOC_CMAKE_DIR}"
                     "${CMAKE_CURRENT_BINARY_DIR}/hwloc_build"
                     EXCLUDE_FROM_ALL)

    # The windows-cmake build exposes the target simply as "hwloc"
    set(HWLOC_TARGET hwloc)

else()
    # ── Linux / macOS: autotools via ExternalProject_Add ─────────────────────
    message(STATUS "hwloc: static build from submodule via autotools (ExternalProject)")

    include(ExternalProject)

    set(_HWLOC_INSTALL_DIR "${CMAKE_CURRENT_BINARY_DIR}/hwloc_install")
    set(_HWLOC_STATIC_LIB  "${_HWLOC_INSTALL_DIR}/lib/libhwloc.a")

    ExternalProject_Add(hwloc_autotools_build
        SOURCE_DIR       "${_HWLOC_SUB}"
        BINARY_DIR       "${CMAKE_CURRENT_BINARY_DIR}/hwloc_autotools_build"
        INSTALL_DIR      "${_HWLOC_INSTALL_DIR}"
        UPDATE_COMMAND   ""
        PATCH_COMMAND    ""
        # cmake/ConfigureHwloc.cmake runs autogen.sh then ./configure
        # --enable-static --disable-shared ensures a .a is produced
        CONFIGURE_COMMAND
            ${CMAKE_COMMAND}
                "-DHWLOC_SOURCE_DIR=${_HWLOC_SUB}"
                "-DHWLOC_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/hwloc_autotools_build"
                "-DHWLOC_INSTALL_DIR=${_HWLOC_INSTALL_DIR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ConfigureHwloc.cmake"
        BUILD_COMMAND    make -j4
        INSTALL_COMMAND  make install
        BUILD_BYPRODUCTS "${_HWLOC_STATIC_LIB}"
    )

    add_library(_hwloc_static STATIC IMPORTED GLOBAL)
    set_target_properties(_hwloc_static PROPERTIES
        IMPORTED_LOCATION             "${_HWLOC_STATIC_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${_HWLOC_INSTALL_DIR}/include"
    )
    add_dependencies(_hwloc_static hwloc_autotools_build)
    set(HWLOC_TARGET _hwloc_static)

endif()

# ─── xbyak (header-only submodule) ────────────────────────────────────────────
set(XBYAK_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/xbyak")
if(NOT EXISTS "${XBYAK_INCLUDE_DIR}/xbyak/xbyak.h")
    message(FATAL_ERROR
        "xbyak submodule not initialised.\n"
        "Run: git submodule update --init --recursive")
endif()
message(STATUS "xbyak: header-only from ${XBYAK_INCLUDE_DIR}")

# ─── Test executable ──────────────────────────────────────────────────────────
add_executable(test_topology test/test_topology.cpp)

target_include_directories(test_topology PRIVATE "${XBYAK_INCLUDE_DIR}")
target_link_libraries(test_topology PRIVATE "${HWLOC_TARGET}")

# ─── CTest integration ────────────────────────────────────────────────────────
enable_testing()
add_test(NAME TopologyTest COMMAND test_topology)
