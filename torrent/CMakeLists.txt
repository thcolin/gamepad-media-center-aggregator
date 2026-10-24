# Torrent engine and torrent_poc. Included by the root when ENABLE_TORRENT=ON,
# or built on its own:
#
#     cmake -B build-torrent torrent -DCMAKE_BUILD_TYPE=Release
#     cmake --build build-torrent -j

cmake_minimum_required(VERSION 3.18)

# C for jech/dht.
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(gmca_torrent C CXX)
    if (NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
    endif ()
else ()
    enable_language(C)
endif ()

set(TORRENT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(TORRENT_INCLUDE ${TORRENT_ROOT}/app/include)
set(TORRENT_SRC ${TORRENT_ROOT}/app/src/torrent)
set(TORRENT_VENDOR ${TORRENT_ROOT}/app/vendor)

set(TORRENT_ENGINE_SOURCES
    ${TORRENT_SRC}/bencode.cpp
    ${TORRENT_SRC}/sha1.cpp
    ${TORRENT_SRC}/crypto.cpp
    ${TORRENT_SRC}/mse.cpp
    ${TORRENT_SRC}/log.cpp
    ${TORRENT_SRC}/util.cpp
    ${TORRENT_SRC}/socket.cpp
    ${TORRENT_SRC}/transport.cpp
    ${TORRENT_SRC}/utp.cpp
    ${TORRENT_SRC}/dht.cpp
    ${TORRENT_SRC}/metadata.cpp
    ${TORRENT_SRC}/http_client.cpp
    ${TORRENT_SRC}/storage.cpp
    ${TORRENT_SRC}/piece_picker.cpp
    ${TORRENT_SRC}/peer.cpp
    ${TORRENT_SRC}/tracker.cpp
    ${TORRENT_SRC}/http_server.cpp
    ${TORRENT_SRC}/engine.cpp
)

# libutp (BEP-29), unmodified, with upstream's own flags kept away from the engine.
set(LIBUTP_DIR ${TORRENT_VENDOR}/libutp)
set(LIBUTP_SOURCES
    ${LIBUTP_DIR}/utp_internal.cpp
    ${LIBUTP_DIR}/utp_utils.cpp
    ${LIBUTP_DIR}/utp_hash.cpp
    ${LIBUTP_DIR}/utp_callbacks.cpp
    ${LIBUTP_DIR}/utp_api.cpp
    ${LIBUTP_DIR}/utp_packedsockaddr.cpp
)
add_library(utp STATIC ${LIBUTP_SOURCES})
target_include_directories(utp PUBLIC ${LIBUTP_DIR})
target_compile_features(utp PRIVATE cxx_std_11)
if (NOT WIN32)
    # Selects the BSD sockets types in utp_types.h, Vita's newlib included.
    target_compile_definitions(utp PRIVATE POSIX)
endif ()
target_compile_options(utp PRIVATE -fno-exceptions -fno-rtti -w)
if (PLATFORM_PSV)
    target_compile_options(utp PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/libutp_vita_compat.h)
endif ()

# jech/dht (BEP-5), unmodified. It makes no socket calls: dht.cpp does the I/O.
set(JECHDHT_DIR ${TORRENT_VENDOR}/jech-dht)
add_library(jechdht STATIC ${JECHDHT_DIR}/dht.c)
target_include_directories(jechdht PUBLIC ${JECHDHT_DIR})
target_compile_options(jechdht PRIVATE -w)

find_package(Threads REQUIRED)

if (PLATFORM_SWITCH OR PLATFORM_PSV OR PLATFORM_PS4)
    # Consoles: mbedtls from the curl portlib, and no CLI.
    set(TORRENT_SHA1_MBEDTLS ON CACHE BOOL "" FORCE)
    if (NOT DEFINED TORRENT_POC)
        set(TORRENT_POC OFF CACHE BOOL "" FORCE)
    endif ()
    if (PLATFORM_PS4)
        # OpenOrbis ships curl without a libcurl.pc.
        find_package(CURL REQUIRED)
    else ()
        find_package(PkgConfig REQUIRED)
        pkg_search_module(CURL REQUIRED libcurl)
    endif ()
else ()
    find_package(CURL REQUIRED)
endif ()

add_library(torrent_engine STATIC ${TORRENT_ENGINE_SOURCES})
target_include_directories(torrent_engine PUBLIC ${TORRENT_INCLUDE})
target_compile_features(torrent_engine PUBLIC cxx_std_17)
target_link_libraries(torrent_engine PUBLIC Threads::Threads)

# __SWITCH__ is not a compiler builtin: borealis sets it only on its own targets.
# Without it socket.cpp and util.cpp build their desktop paths and crash on Switch.
if (PLATFORM_SWITCH)
    target_compile_definitions(torrent_engine PRIVATE __SWITCH__)
endif ()
target_link_libraries(torrent_engine PRIVATE utp)
target_link_libraries(torrent_engine PRIVATE jechdht)
if (PLATFORM_SWITCH OR PLATFORM_PSV)
    target_include_directories(torrent_engine PUBLIC ${CURL_INCLUDE_DIRS})
    target_link_directories(torrent_engine PUBLIC ${CURL_LIBRARY_DIRS})
    target_link_libraries(torrent_engine PUBLIC ${CURL_LIBRARIES})
else ()
    target_link_libraries(torrent_engine PUBLIC CURL::libcurl)
endif ()

# SHA-1 default: CommonCrypto on Apple, the bundled one elsewhere.
option(TORRENT_SHA1_OPENSSL "Use OpenSSL libcrypto for SHA-1" OFF)
option(TORRENT_SHA1_MBEDTLS "Use mbedtls for SHA-1 (console seam)" OFF)
if (TORRENT_SHA1_OPENSSL)
    find_package(OpenSSL REQUIRED)
    target_compile_definitions(torrent_engine PRIVATE TORRENT_SHA1_OPENSSL)
    target_link_libraries(torrent_engine PUBLIC OpenSSL::Crypto)
elseif (TORRENT_SHA1_MBEDTLS)
    target_compile_definitions(torrent_engine PRIVATE TORRENT_SHA1_MBEDTLS)
    # Already linked through curl; named so a standalone link resolves too.
    target_link_libraries(torrent_engine PUBLIC mbedcrypto)
endif ()

option(TORRENT_POC "Build the torrent_poc executable" ON)
if (TORRENT_POC)
    add_executable(torrent_poc ${CMAKE_CURRENT_SOURCE_DIR}/poc.cpp)
    target_link_libraries(torrent_poc PRIVATE torrent_engine)
    target_compile_features(torrent_poc PRIVATE cxx_std_17)
endif ()
