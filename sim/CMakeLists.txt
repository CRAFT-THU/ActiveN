cmake_minimum_required(VERSION 3.27)

project(koneko_sim)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(verilator HINTS $ENV{VERILATOR_ROOT})
set(SRCS src/main.cpp)
add_executable(sim ${SRCS})

verilate(
    sim
    SOURCES ../Core.sv
    TRACE_FST
    VERILATOR_ARGS -Wall -Wno-fatal -Wno-DECLFILENAME --trace-params --trace-structs --top-module Core -DRANDOMIZE_REG_INIT -DRANDOMIZE_MEM_INIT
)