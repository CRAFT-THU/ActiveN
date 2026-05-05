cmake_minimum_required(VERSION 3.25)

project(koneko_sim)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD_REQUIRED ON)

# PGO options
option(PGO_GENERATE "Build with PGO instrumentation (profile generation)" OFF)
option(PGO_USE "Build with PGO optimization (profile use)" OFF)
set(PGO_PROFILE_DIR "${CMAKE_BINARY_DIR}/pgo-profiles" CACHE STRING "Directory for PGO profile data")
option(USE_LTO "Enable ThinLTO" OFF)
option(USE_BOLT "Apply BOLT post-link optimization (instrumentation mode)" ON)

if(PGO_GENERATE AND PGO_USE)
  message(FATAL_ERROR "Cannot enable both PGO_GENERATE and PGO_USE simultaneously")
endif()

if(PGO_GENERATE AND USE_BOLT)
  message(FATAL_ERROR "Cannot enable both PGO_GENERATE and USE_BOLT simultaneously (instrumented binaries cannot be BOLTed)")
endif()

if(PGO_GENERATE)
  add_compile_options(-fprofile-generate=${PGO_PROFILE_DIR})
  add_link_options(-fprofile-generate=${PGO_PROFILE_DIR})
  message(STATUS "PGO: Generating profiles to ${PGO_PROFILE_DIR}")
elseif(PGO_USE)
  if(EXISTS "${PGO_PROFILE_DIR}/default.profdata")
    # Clang merged profile
    add_compile_options(-fprofile-use=${PGO_PROFILE_DIR}/default.profdata)
    add_link_options(-fprofile-use=${PGO_PROFILE_DIR}/default.profdata)
  else()
    # GCC-style auto-detect
    add_compile_options(-fprofile-use=${PGO_PROFILE_DIR} -fprofile-correction)
    add_link_options(-fprofile-use=${PGO_PROFILE_DIR})
  endif()
  message(STATUS "PGO: Using profiles from ${PGO_PROFILE_DIR}")
endif()

if(USE_LTO)
  add_compile_options(-flto=thin)
  add_link_options(-flto=thin)
  message(STATUS "ThinLTO enabled")
endif()

if(USE_BOLT)
  find_program(LLVM_BOLT llvm-bolt REQUIRED)
  add_link_options(-Wl,--emit-relocs)
  message(STATUS "BOLT: post-link optimization enabled (${LLVM_BOLT})")
endif()

# Apply BOLT instrumentation-based optimization as a post-build step.
function(apply_bolt TARGET BOLT_PAYLOAD BOLT_MAX_CYCLES)
  if(NOT USE_BOLT)
    return()
  endif()
  # Extra positional args become BOLT_EXTRA_ARGS
  set(BOLT_RUN_ARGS ${ARGN})
  set(FDATA "${CMAKE_BINARY_DIR}/${TARGET}.fdata")
  add_custom_command(TARGET ${TARGET} POST_BUILD
    COMMENT "BOLT: optimizing ${TARGET}"
    # Instrument
    COMMAND ${LLVM_BOLT} $<TARGET_FILE:${TARGET}>
            --instrument --instrumentation-file=${FDATA}
            -o $<TARGET_FILE:${TARGET}>.inst
    # Profile (ignore non-zero exit from simulation timeout)
    COMMAND sh -c "$<TARGET_FILE:${TARGET}>.inst ${BOLT_PAYLOAD} --max-cycles ${BOLT_MAX_CYCLES} ${BOLT_RUN_ARGS} > /dev/null 2>&1 || true"
    # Optimize
    COMMAND ${LLVM_BOLT} $<TARGET_FILE:${TARGET}>
            -o $<TARGET_FILE:${TARGET}>.bolted
            -data=${FDATA}
            -reorder-blocks=ext-tsp
            -reorder-functions=hfsort
            -split-functions
            -split-all-cold
    # Swap
    COMMAND ${CMAKE_COMMAND} -E rename $<TARGET_FILE:${TARGET}>.bolted $<TARGET_FILE:${TARGET}>
    # Cleanup
    COMMAND ${CMAKE_COMMAND} -E rm -f $<TARGET_FILE:${TARGET}>.inst ${FDATA}
    VERBATIM
  )
endfunction()



include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
find_package(verilator HINTS $ENV{VERILATOR_ROOT})

find_library(DRAMSIM3 dramsim3)

# Generated RTL directories
set(GENERATED_CORE_DIR ${CMAKE_SOURCE_DIR}/../generated/core)
set(GENERATED_SYSTEM_DIR ${CMAKE_SOURCE_DIR}/../generated/system)

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

# BOLT profiling payloads
set(BOLT_SINGLE_PAYLOAD ${CMAKE_SOURCE_DIR}/payloads/c/sha256_test.bin)
set(BOLT_MULTI_PAYLOAD ${CMAKE_SOURCE_DIR}/payloads/sys/random_noc_test.bin)
set(BOLT_SINGLE_CYCLES 5000000)
set(BOLT_MULTI_CYCLES 100000)

set(VERILATOR_COMMON_ARGS -O3 -Wall -Wno-fatal -Wno-DECLFILENAME --trace-params --trace-structs -I${CVFPU_DIR}/src/common_cells/include -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-UNSIGNED -Wno-CASEINCOMPLETE -Wno-UNUSEDSIGNAL -Wno-UNUSEDPARAM -Wno-UNDRIVEN -Wno-PINMISSING)
set(VERILATOR_CORE_ARGS ${VERILATOR_COMMON_ARGS} --top-module Core)
set(VERILATOR_SYSTEM_ARGS ${VERILATOR_COMMON_ARGS} --top-module System --threads 8)

# Single-core simulation driver (no DRAM dependency)
add_executable(sim_single
    src/single.cpp
)

verilate(
    sim_single
  SOURCES ${CVFPU_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/../Aux.sv
    PREFIX rtl
    TRACE_FST
    DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/verilated
    INCLUDE_DIRS ${GENERATED_CORE_DIR}
    VERILATOR_ARGS "-f" "${GENERATED_CORE_DIR}/filelist.f" ${VERILATOR_CORE_ARGS}
)
apply_bolt(sim_single ${BOLT_SINGLE_PAYLOAD} ${BOLT_SINGLE_CYCLES})

# System-level simulation driver (multicore + NoC)
# Read config from System.config.json (generated by Mill/Chisel elaboration)
set(SYSTEM_CONFIG_JSON ${GENERATED_SYSTEM_DIR}/System.config.json)
if(NOT EXISTS ${SYSTEM_CONFIG_JSON})
  message(FATAL_ERROR "System.config.json not found at ${SYSTEM_CONFIG_JSON}. Run Mill elaboration first.")
endif()

# Read numPU and numMC from JSON for gen_system_header.py and Verilator
file(READ ${SYSTEM_CONFIG_JSON} SYSTEM_CONFIG_CONTENT)
string(JSON SYSTEM_NUM_PU GET ${SYSTEM_CONFIG_CONTENT} "numPU")
string(JSON SYSTEM_NUM_MC GET ${SYSTEM_CONFIG_CONTENT} "numMC")

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/system_config.h
           ${CMAKE_CURRENT_BINARY_DIR}/hard_backend.h
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/gen_system_header.py
            ${SYSTEM_CONFIG_JSON} ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${SYSTEM_CONFIG_JSON}
            ${CMAKE_CURRENT_SOURCE_DIR}/gen_system_header.py
    COMMENT "Generating system_config.h and hard_backend.h from System.config.json"
)
add_custom_target(gen_system_headers DEPENDS
    ${CMAKE_CURRENT_BINARY_DIR}/system_config.h
    ${CMAKE_CURRENT_BINARY_DIR}/hard_backend.h
)

# Hard backend: Verilated System RTL
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/hard_backend_stub.cpp "// stub for verilator linkage\n")
add_library(hard_backend_lib STATIC
    ${CMAKE_CURRENT_BINARY_DIR}/hard_backend_stub.cpp
)
verilate(
  hard_backend_lib
    SOURCES ${CVFPU_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/../Aux.sv
    PREFIX sys_rtl
    TRACE_FST
    DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/sys_verilated
    INCLUDE_DIRS ${GENERATED_SYSTEM_DIR}
    VERILATOR_ARGS "-f" "${GENERATED_SYSTEM_DIR}/filelist.f" ${VERILATOR_SYSTEM_ARGS}
)

# Soft backend: many Core instances + C++ NoC model
# Uses Core.sv from generated/system so config matches the hard backend exactly.
add_library(soft_backend_lib STATIC
    src/soft_backend.cpp
)

verilate(
  soft_backend_lib
    SOURCES ${CVFPU_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/../Aux.sv
    PREFIX soft_rtl
    TRACE_FST
    DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/soft_verilated
    INCLUDE_DIRS ${GENERATED_SYSTEM_DIR}
    VERILATOR_ARGS "-f" "${GENERATED_SYSTEM_DIR}/filelist.f" ${VERILATOR_CORE_ARGS}
)

# Unified binary: sim_system --soft / --hard / --soft --hard (cosim)
add_executable(sim_system
  src/system.cpp
)
add_dependencies(sim_system gen_system_headers)
add_dependencies(soft_backend_lib gen_system_headers)
target_link_libraries(sim_system PRIVATE
  soft_backend_lib
  hard_backend_lib
  ${DRAMSIM3}
)
apply_bolt(sim_system "${BOLT_MULTI_PAYLOAD}" "${BOLT_MULTI_CYCLES}" "--hard")
