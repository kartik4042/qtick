cmake_minimum_required(VERSION 3.15)
project(qtick CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Optimization flags for release
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -flto -funroll-loops -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -fsanitize=address,undefined")

# Default to Release if not specified
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "CXX Flags: ${CMAKE_CXX_FLAGS_${CMAKE_BUILD_TYPE}}")

# Find dependencies
find_package(Threads REQUIRED)
find_package(spdlog REQUIRED)

# Find hiredis
find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h)
find_library(HIREDIS_LIBRARY hiredis)

if(NOT HIREDIS_INCLUDE_DIR OR NOT HIREDIS_LIBRARY)
  message(FATAL_ERROR "hiredis not found. Install: sudo dnf install hiredis-devel")
endif()

message(STATUS "Found hiredis: ${HIREDIS_LIBRARY}")

# kdb+ C API - assumes k.h and c.o are in kdb_api/
# Download from: https://github.com/KxSystems/kdb/tree/master/c/c
set(KDB_DIR "${CMAKE_SOURCE_DIR}/kdb_api" CACHE PATH "Path to kdb+ C API")

if(NOT EXISTS "${KDB_DIR}/k.h")
  message(WARNING "k.h not found in ${KDB_DIR}. Download from KxSystems/kdb repo.")
endif()

# Common library (header-only)
add_library(qtick_common INTERFACE)
target_include_directories(qtick_common INTERFACE 
  ${CMAKE_SOURCE_DIR}
  ${KDB_DIR}
)

# Feed simulator
add_executable(feed_sim src/feed_sim/main.cpp)
target_link_libraries(feed_sim PRIVATE 
  qtick_common 
  ${HIREDIS_LIBRARY}
  Threads::Threads
)
target_include_directories(feed_sim PRIVATE ${HIREDIS_INCLUDE_DIR})

# Ingest bridge
add_executable(bridge src/ingest_bridge/main.cpp ${KDB_DIR}/c.o)
target_link_libraries(bridge PRIVATE
  qtick_common
  ${HIREDIS_LIBRARY}
  spdlog::spdlog
  Threads::Threads
  dl  # for kdb+ IPC
)
target_include_directories(bridge PRIVATE ${HIREDIS_INCLUDE_DIR})

# Add -fno-exceptions -fno-rtti for hot path optimization
target_compile_options(bridge PRIVATE -fno-exceptions -fno-rtti)
target_compile_options(feed_sim PRIVATE -fno-exceptions -fno-rtti)

# Install targets
install(TARGETS feed_sim bridge RUNTIME DESTINATION bin)
install(FILES q/rdb.q DESTINATION share/qtick)

message(STATUS "")
message(STATUS "Build commands:")
message(STATUS "  mkdir build && cd build")
message(STATUS "  cmake .. -DCMAKE_BUILD_TYPE=Release")
message(STATUS "  make -j$(nproc)")
message(STATUS "")
message(STATUS "Run:")
message(STATUS "  q ../q/rdb.q -p 5010")
message(STATUS "  redis-server /etc/redis/redis.conf")
message(STATUS "  ./bridge /var/run/redis/redis.sock localhost 5010")
message(STATUS "  ./feed_sim /var/run/redis/redis.sock")