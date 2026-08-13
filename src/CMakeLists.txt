# -- building like cmake -B build -G "Visual Studio 17 2022" -A x64 -DBOOST_ROOT="C:/tools/libs/boost_1_88_0" -DLIVE555_ROOT="C:/tools/libs/live"
# on linux remember to build Live555 with linux-no-openssl
# or you could find and link to openssl here.. whatever
# boost libs: C:\tools\libs\boost_1_88_0\stage\lib
# headers in the root dir no problem

cmake_minimum_required(VERSION 3.20)
project(tutk_rtsp_bridge CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# --------------------------------------------------------------------------
# Tests - deliberately first and independent of everything below.
#
# tests/test_scrambler.cc, tests/test_avapi.cc, tests/test_framesync.cc, and
# tests/test_audiosequencer.cc only touch scrambler.hh and AVAPI.hh (both
# header-only, and AVAPI.hh only additionally needs Logger.hh - no boost or
# live555), so
# `cmake -B build -DBUILD_MAIN_EXECUTABLE=OFF && cmake --build build && ctest --test-dir build`
# works with nothing but a C++23 compiler - no LIVE555_ROOT, no Boost.
# See tests/test_util.hh for why there's no test framework dependency either.
# --------------------------------------------------------------------------
option(BUILD_TESTS "Build the test executables" ON)
if(BUILD_TESTS)
    enable_testing()

    add_executable(test_scrambler tests/test_scrambler.cc)
    target_include_directories(test_scrambler PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME scrambler COMMAND test_scrambler)

    add_executable(test_avapi tests/test_avapi.cc)
    target_include_directories(test_avapi PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME avapi COMMAND test_avapi)

    add_executable(test_framesync tests/test_framesync.cc)
    target_include_directories(test_framesync PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME framesync COMMAND test_framesync)

    add_executable(test_audiosequencer tests/test_audiosequencer.cc)
    target_include_directories(test_audiosequencer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME audiosequencer COMMAND test_audiosequencer)
endif()

# --------------------------------------------------------------------------
# Main executable - everything below this point needs Boost + live555.
# Turn off if you only want to build/run the tests above.
# --------------------------------------------------------------------------
option(BUILD_MAIN_EXECUTABLE "Build the tutk_rtsp_bridge executable (needs Boost + live555)" ON)
if(BUILD_MAIN_EXECUTABLE)

# IOTCPacket.hh, scrambler.hh, AVAPI.hh, Logger.hh, and the Live555Bridge.hh
# quartet (FrameQueue/TutkH264Source/TutkServerMediaSubsession/
# TutkAudioServerMediaSubsession) are all header-only now, so they don't get
# their own entries here.
set(SOURCES
    IOTCCameraClient.cc
    main.cc
)

add_executable(tutk_rtsp_bridge ${SOURCES})
target_include_directories(tutk_rtsp_bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# --------------------------------------------------------------------------
# Boost.Asio -- arent we just including the entirety of boost? 
# Well... program options is actually a compiled boost module
# did not realise
# --------------------------------------------------------------------------
#find_package(Boost REQUIRED)
#target_link_libraries(tutk_rtsp_bridge PRIVATE Boost::boost)

find_package(Boost REQUIRED COMPONENTS program_options)
target_link_libraries(tutk_rtsp_bridge PRIVATE Boost::boost Boost::program_options)

find_package(Threads REQUIRED)
target_link_libraries(tutk_rtsp_bridge PRIVATE Threads::Threads)

# --------------------------------------------------------------------------
# live555 - point LIVE555_ROOT at wherever you already built/installed it.
#
# Default layout assumed here is the classic in-tree build (genMakefiles +
# make), i.e.:
#   ${LIVE555_ROOT}/liveMedia/{include,libliveMedia.a}
#   ${LIVE555_ROOT}/groupsock/{include,libgroupsock.a}
#   ${LIVE555_ROOT}/UsageEnvironment/{include,libUsageEnvironment.a}
#   ${LIVE555_ROOT}/BasicUsageEnvironment/{include,libBasicUsageEnvironment.a}
#
# If yours is laid out differently (e.g. a single `make install` prefix with
# lib/ and include/ merged), skip LIVE555_ROOT and set LIVE555_INCLUDE_DIRS /
# LIVE555_LIBRARIES directly instead - both are cache variables so they can
# be set from the command line or a CMakeUserPresets.json without editing
# this file.
# --------------------------------------------------------------------------
set(LIVE555_ROOT "" CACHE PATH "Root of a classic in-tree live555 build")

set(_live555_components UsageEnvironment BasicUsageEnvironment groupsock liveMedia)
set(_live555_default_includes "")

foreach(comp ${_live555_components})
    if(LIVE555_ROOT)
        list(APPEND _live555_default_includes "${LIVE555_ROOT}/${comp}/include")
    endif()
endforeach()

set(LIVE555_INCLUDE_DIRS "${_live555_default_includes}" CACHE STRING "live555 include directories")

# Find libraries dynamically across Linux (.a) and Windows (.lib)
set(LIVE555_LIBRARIES "")
foreach(comp liveMedia groupsock BasicUsageEnvironment UsageEnvironment)
    # Search both the component subfolder and modern lib/ directories
    find_library(LIVE555_${comp}_LIB 
        NAMES ${comp} lib${comp}
        PATHS "${LIVE555_ROOT}/${comp}" "${LIVE555_ROOT}/lib"
        NO_DEFAULT_PATH
    )
    if(LIVE555_${comp}_LIB)
        list(APPEND LIVE555_LIBRARIES "${LIVE555_${comp}_LIB}")
    endif()
endforeach()

if(NOT LIVE555_LIBRARIES OR NOT LIVE555_INCLUDE_DIRS)
    message(FATAL_ERROR "Could not find live555 libraries or headers. Set LIVE555_ROOT to your live555 build directory.")
endif()

target_include_directories(tutk_rtsp_bridge PRIVATE ${LIVE555_INCLUDE_DIRS})
target_link_libraries(tutk_rtsp_bridge PRIVATE ${LIVE555_LIBRARIES})

# --------------------------------------------------------------------------
# Windows
# --------------------------------------------------------------------------
if(WIN32)
    target_link_libraries(tutk_rtsp_bridge PRIVATE ws2_32)
endif()

endif() # BUILD_MAIN_EXECUTABLE
