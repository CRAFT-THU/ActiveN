cmake_minimum_required(VERSION 3.27)

project(koneko_sim)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD_REQUIRED ON)

include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
find_package(verilator HINTS $ENV{VERILATOR_ROOT})
set(SRCS
    src/meow.cpp
    src/aux.cpp
)

# include_directories(${CMAKE_CURRENT_LIST_DIR}/booksim2/src)
# include_directories(${CMAKE_CURRENT_LIST_DIR}/booksim2/src/arbiters)
# include_directories(${CMAKE_CURRENT_LIST_DIR}/booksim2/src/allocators)
# include_directories(${CMAKE_CURRENT_LIST_DIR}/booksim2/src/routers)
# include_directories(${CMAKE_CURRENT_LIST_DIR}/booksim2/src/networks)
# include_directories(${CMAKE_CURRENT_LIST_DIR}/booksim2/src/power)

# FILE(GLOB_RECURSE BOOKSIM_SRCS booksim2/src/*.cpp booksim2/src/lex.yy.c booksim2/src/y.tab.c)
find_library(DRAMSIM3 dramsim3)

add_executable(sim ${SRCS})

verilate(
    sim
    SOURCES ${CMAKE_SOURCE_DIR}/../Core.sv ${CMAKE_SOURCE_DIR}/../Aux.sv
    PREFIX rtl
    TRACE_FST
    DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/verilated
    VERILATOR_ARGS -Wall -Wno-fatal -Wno-DECLFILENAME --trace-params --trace-structs --top-module Core -DRANDOMIZE_REG_INIT -DRANDOMIZE_MEM_INIT
)

target_link_libraries(sim PRIVATE ${DRAMSIM3})
