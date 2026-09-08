cmake_minimum_required(VERSION 3.20)
project(sqlite_wal_reset_repro C)

set(SQLITE_AMALGAMATION_DIR "" CACHE PATH
    "Directory containing the selected sqlite3.c and sqlite3.h")

if(NOT EXISTS "${SQLITE_AMALGAMATION_DIR}/sqlite3.c")
    message(FATAL_ERROR
        "Set SQLITE_AMALGAMATION_DIR to an unpacked SQLite amalgamation")
endif()

find_package(Threads REQUIRED)

add_library(sqlite3-under-test STATIC
    "${SQLITE_AMALGAMATION_DIR}/sqlite3.c"
)
target_include_directories(sqlite3-under-test PUBLIC
    "${SQLITE_AMALGAMATION_DIR}"
)
target_compile_definitions(sqlite3-under-test PRIVATE
    SQLITE_THREADSAFE=1
)
target_compile_features(sqlite3-under-test PUBLIC c_std_11)

add_executable(wal-reset-repro
    src/wal_reset_repro.c
)
target_compile_features(wal-reset-repro PRIVATE c_std_11)
target_compile_options(wal-reset-repro PRIVATE
    -Wall
    -Wextra
    -Werror
)
target_link_libraries(wal-reset-repro PRIVATE
    sqlite3-under-test
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

if(NOT APPLE)
    target_link_libraries(wal-reset-repro PRIVATE m)
endif()
