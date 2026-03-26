cmake_minimum_required(VERSION 3.25)

project(koneko_sim)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD_REQUIRED ON)

if (DEFINED ENV{AN_PIPE_CNT})
  add_compile_definitions(AN_PIPE_CNT=$ENV{AN_PIPE_CNT})
else()
  add_compile_definitions(AN_PIPE_CNT=2)
endif()

include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
find_package(verilator HINTS $ENV{VERILATOR_ROOT})
set(SRCS
    src/meow.cpp
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

# cvfpu source files
set(CVFPU_DIR ${CMAKE_SOURCE_DIR}/../dep/cvfpu)
set(CVFPU_SOURCES
    ${CVFPU_DIR}/src/fpnew_pkg.sv
    ${CVFPU_DIR}/src/fpnew_cast_multi.sv
    ${CVFPU_DIR}/src/fpnew_classifier.sv
    ${CVFPU_DIR}/src/fpnew_fma.sv
    ${CVFPU_DIR}/src/fpnew_fma_multi.sv
    ${CVFPU_DIR}/src/fpnew_noncomp.sv
    ${CVFPU_DIR}/src/fpnew_opgroup_block.sv
    ${CVFPU_DIR}/src/fpnew_opgroup_fmt_slice.sv
    ${CVFPU_DIR}/src/fpnew_opgroup_multifmt_slice.sv
    ${CVFPU_DIR}/src/fpnew_rounding.sv
    ${CVFPU_DIR}/src/fpnew_top.sv
    ${CVFPU_DIR}/src/common_cells/src/cf_math_pkg.sv
    ${CVFPU_DIR}/src/common_cells/src/lzc.sv
    ${CVFPU_DIR}/src/common_cells/src/rr_arb_tree.sv
)

set(VERILATOR_COMMON_ARGS -Wall -Wno-fatal -Wno-DECLFILENAME --trace-params --trace-structs --top-module Core -DRANDOMIZE_REG_INIT -DRANDOMIZE_MEM_INIT -I${CVFPU_DIR}/src/common_cells/include -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-UNSIGNED -Wno-CASEINCOMPLETE -Wno-UNUSEDSIGNAL -Wno-UNUSEDPARAM -Wno-UNDRIVEN -Wno-PINMISSING)

verilate(
    sim
    SOURCES ${CVFPU_SOURCES} ${CMAKE_SOURCE_DIR}/../Core.sv ${CMAKE_SOURCE_DIR}/../Aux.sv
    PREFIX rtl
    TRACE_FST
    DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/verilated
    VERILATOR_ARGS ${VERILATOR_COMMON_ARGS}
)

target_link_libraries(sim PRIVATE ${DRAMSIM3})

# Single-core simulation driver (no DRAM dependency)
add_executable(sim_single
    src/single.cpp
)

verilate(
    sim_single
    SOURCES ${CVFPU_SOURCES} ${CMAKE_SOURCE_DIR}/../Core.sv ${CMAKE_SOURCE_DIR}/../Aux.sv
    PREFIX rtl
    TRACE_FST
    DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/verilated
    VERILATOR_ARGS ${VERILATOR_COMMON_ARGS}
)
