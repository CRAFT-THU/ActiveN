cmake_minimum_required(VERSION 3.27)

project(koneko_sim)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD_REQUIRED ON)

include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

find_package(verilator HINTS $ENV{VERILATOR_ROOT})
set(SRCS
    src/main.cpp
)
add_executable(sim ${SRCS})

verilate(
    sim
    SOURCES ${CMAKE_SOURCE_DIR}/../Core.sv
    PREFIX rtl
    TRACE_FST
    DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/verilated
    VERILATOR_ARGS -Wall -Wno-fatal -Wno-DECLFILENAME --trace-params --trace-structs --top-module Core -DRANDOMIZE_REG_INIT -DRANDOMIZE_MEM_INIT
)