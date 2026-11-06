cmake_minimum_required(VERSION 3.16)
project(CampusEvents VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(CE_BUILD_TESTS "Build the unit / integration tests" ON)
option(CE_FORCE_VENDORED_SQLITE "Always compile the bundled SQLite instead of using a system copy" OFF)

find_package(Threads REQUIRED)

# SQLite: use the system library when available, otherwise the bundled amalgamation,
# so a plain `cmake -B build && cmake --build build` works on Windows, macOS and Linux.
set(CE_SQLITE_TARGET "")
if(NOT CE_FORCE_VENDORED_SQLITE)
  find_package(SQLite3 QUIET)
  if(SQLite3_FOUND)
    set(CE_SQLITE_TARGET SQLite::SQLite3)
    message(STATUS "CampusEvents: using system SQLite ${SQLite3_VERSION}")
  endif()
endif()
if(NOT CE_SQLITE_TARGET)
  message(STATUS "CampusEvents: building bundled SQLite")
  add_library(ce_sqlite STATIC third_party/sqlite3/sqlite3.c)
  target_include_directories(ce_sqlite SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite3)
  target_compile_definitions(ce_sqlite PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION=1)
  target_link_libraries(ce_sqlite PUBLIC Threads::Threads)
  set_target_properties(ce_sqlite PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
  if(NOT MSVC)
    target_compile_options(ce_sqlite PRIVATE -w)
  endif()
  set(CE_SQLITE_TARGET ce_sqlite)
endif()

# Core library: everything except main()
add_library(campusevents_core STATIC
  src/util.cpp
  src/db.cpp
  src/repository.cpp
  src/service.cpp
  src/seed.cpp
  src/api.cpp)
target_include_directories(campusevents_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(campusevents_core SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(campusevents_core PUBLIC ${CE_SQLITE_TARGET} Threads::Threads)
# cpp-httplib's default accept queue is only 5 connections: a burst of students opening the page at the
# same moment would get some connections reset. A deeper queue lets the worker threads catch up.
target_compile_definitions(campusevents_core PUBLIC CPPHTTPLIB_LISTEN_BACKLOG=256)
if(WIN32)
  target_link_libraries(campusevents_core PUBLIC ws2_32)
  target_compile_definitions(campusevents_core PUBLIC _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()
if(MSVC)
  target_compile_options(campusevents_core PUBLIC /W4 /utf-8 /bigobj)
else()
  target_compile_options(campusevents_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(campusevents src/main.cpp)
target_link_libraries(campusevents PRIVATE campusevents_core)
target_compile_definitions(campusevents PRIVATE CE_VERSION="${PROJECT_VERSION}")

if(CE_BUILD_TESTS)
  enable_testing()
  add_executable(campusevents_tests tests/test_main.cpp)
  target_link_libraries(campusevents_tests PRIVATE campusevents_core)
  add_test(NAME campusevents_tests COMMAND campusevents_tests)
endif()
