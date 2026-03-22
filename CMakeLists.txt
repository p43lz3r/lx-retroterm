cmake_minimum_required(VERSION 3.16)
project(ttcore-port C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Strict warnings — Google C Style Guide
add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter -g)

include_directories(include)

# ---------------------------------------------------------------------------
# libttcore — static library
# ---------------------------------------------------------------------------
set(TTCORE_SOURCES
    src/ttcore_buffer.c
    src/vtparse.c
    src/vtcharset.c
    src/vtmouse.c
    src/vtcolor.c
    src/libttcore.c
    src/ttcore_io.c
    src/ttcore_transfer.c
)

add_library(ttcore STATIC ${TTCORE_SOURCES})

# iconv is part of glibc on Linux; no separate -liconv needed

# ---------------------------------------------------------------------------
# GTK3 GUI
# ---------------------------------------------------------------------------
add_subdirectory(gui)
