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
option(USE_MIMALLOC "Link mimalloc as the global memory allocator" ON)
option(ENABLE_HARD_BACKEND "Build the Verilated full-system hard backend" ON)

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
  # Flatten the list for sh -c (avoid semicolon-separation when expanding).
  string(REPLACE ";" " " BOLT_RUN_ARGS_STR "${BOLT_RUN_ARGS}")
  set(FDATA "${CMAKE_BINARY_DIR}/${TARGET}.fdata")
  add_custom_command(TARGET ${TARGET} POST_BUILD
    COMMENT "BOLT: optimizing ${TARGET}"
    # Instrument
    COMMAND ${LLVM_BOLT} $<TARGET_FILE:${TARGET}>
            --instrument --instrumentation-file=${FDATA}
            -o $<TARGET_FILE:${TARGET}>.inst
    # Profile (ignore non-zero exit from simulation timeout)
    COMMAND sh -c "$<TARGET_FILE:${TARGET}>.inst --max-cycles ${BOLT_MAX_CYCLES} ${BOLT_RUN_ARGS_STR} ${BOLT_PAYLOAD} > /dev/null 2>&1 || true"
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
find_package(Ramulator 2.1 CONFIG REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(RAMULATOR_CONFIG_PY ${CMAKE_CURRENT_SOURCE_DIR}/ramulator_config.py)
set(RAMULATOR_CONFIG_YAML ${CMAKE_CURRENT_BINARY_DIR}/ramulator.yaml)
add_custom_command(
    OUTPUT ${RAMULATOR_CONFIG_YAML}
    COMMAND ${Python3_EXECUTABLE} -m ramulator export
            ${RAMULATOR_CONFIG_PY} -o ${RAMULATOR_CONFIG_YAML}
    DEPENDS ${RAMULATOR_CONFIG_PY}
    COMMENT "Exporting ActiveN Ramulator configuration"
    VERBATIM
)
add_custom_target(gen_ramulator_config DEPENDS ${RAMULATOR_CONFIG_YAML})

# ---------------------------------------------------------------------------
# mimalloc global allocator (static override)
#
# The mimalloc docs recommend linking the single mimalloc object file as the
# FIRST object of the final program for a reliable global override of
# malloc/free and operator new/delete. Unlike linking the shared library, this
# does not depend on DT_NEEDED ordering or --as-needed behavior. nix splits
# mimalloc into dev/non-dev outputs and ships mimalloc.o next to libmimalloc.so
# in the non-dev output, which find_library resolves; we derive mimalloc.o from
# that directory (find_package's MIMALLOC_OBJECT_DIR points at the wrong output
# under nix). Build with -DUSE_MIMALLOC=OFF to use the system allocator.
# ---------------------------------------------------------------------------
if(USE_MIMALLOC)
  find_library(MIMALLOC_SO NAMES mimalloc REQUIRED)
  get_filename_component(MIMALLOC_LIBDIR "${MIMALLOC_SO}" DIRECTORY)
  set(MIMALLOC_OBJ "${MIMALLOC_LIBDIR}/mimalloc.o")
  if(NOT EXISTS "${MIMALLOC_OBJ}")
    message(FATAL_ERROR "USE_MIMALLOC=ON but mimalloc.o not found at ${MIMALLOC_OBJ}")
  endif()
  set_source_files_properties(${MIMALLOC_OBJ} PROPERTIES
    EXTERNAL_OBJECT TRUE GENERATED FALSE)
  # mimalloc.o (linked first) already provides operator new/delete overrides
  # that route straight to mi_new/mi_free, so no C++ header include is needed.
  message(STATUS "mimalloc: static override via first-object ${MIMALLOC_OBJ}")
else()
  set(MIMALLOC_OBJ "")
endif()

# Generated RTL directories
set(GENERATED_CORE_DIR ${CMAKE_SOURCE_DIR}/../generated/core)
set(GENERATED_SYSTEM_DIR ${CMAKE_SOURCE_DIR}/../generated/system)

set(CORE_CONFIG_JSON ${GENERATED_CORE_DIR}/Core.config.json)
if(NOT EXISTS ${CORE_CONFIG_JSON})
  message(FATAL_ERROR "Core.config.json not found at ${CORE_CONFIG_JSON}. Run Mill elaboration first.")
endif()
file(READ ${CORE_CONFIG_JSON} CORE_CONFIG_CONTENT)
string(JSON CORE_MEM_BUS_WIDTH GET ${CORE_CONFIG_CONTENT} "memBusWidth")

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
# SNN cosim workload: two distinct DRAM images generated by datagen.
# See AGENTS.md for the regeneration command. apply_bolt appends BOLT_PAYLOAD
# at the end of the command, so BOLT_PAYLOAD = dram.1 and the extra args
# below carry dram.0 to keep the MC0/MC1 ordering correct.
set(BOLT_MULTI_DRAM0 ${CMAKE_SOURCE_DIR}/../copilot/tmp/snn_shuffle/dram.0)
set(BOLT_MULTI_PAYLOAD ${CMAKE_SOURCE_DIR}/../copilot/tmp/snn_shuffle/dram.1)
set(BOLT_SINGLE_CYCLES 5000000)
set(BOLT_MULTI_CYCLES 500000)

set(VERILATOR_COMMON_ARGS -O3 -Wall -Wno-fatal -Wno-DECLFILENAME --trace-params --trace-structs -I${CVFPU_DIR}/src/common_cells/include -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-UNSIGNED -Wno-CASEINCOMPLETE -Wno-UNUSEDSIGNAL -Wno-UNUSEDPARAM -Wno-UNDRIVEN -Wno-PINMISSING)
set(VERILATOR_CORE_ARGS ${VERILATOR_COMMON_ARGS} --top-module Core)
set(VERILATOR_SYSTEM_ARGS ${VERILATOR_COMMON_ARGS} --top-module System --threads 8)

# ---------------------------------------------------------------------------
# Boot ROM
#
# The core resets into the peripheral boot-ROM window (physical 0x60000000).
# Assemble sim/bootrom.S to a raw binary and #embed it into devices.cpp as the
# built-in default image; --bootrom overrides it at runtime.
# ---------------------------------------------------------------------------
set(RISCV_PREFIX "riscv64-unknown-none-elf-" CACHE STRING
    "RISC-V bare-metal toolchain prefix for the boot ROM")
set(BOOTROM_SRC ${CMAKE_CURRENT_SOURCE_DIR}/bootrom.S)
set(BOOTROM_ELF ${CMAKE_CURRENT_BINARY_DIR}/bootrom.elf)
set(BOOTROM_BIN ${CMAKE_CURRENT_BINARY_DIR}/bootrom.bin)
add_custom_command(
    OUTPUT ${BOOTROM_BIN}
    COMMAND ${RISCV_PREFIX}gcc -march=rv32i -mabi=ilp32 -nostdlib -nostartfiles
            -Wl,-Ttext=0x60000000 -o ${BOOTROM_ELF} ${BOOTROM_SRC}
    COMMAND ${RISCV_PREFIX}objcopy -O binary ${BOOTROM_ELF} ${BOOTROM_BIN}
    DEPENDS ${BOOTROM_SRC}
    COMMENT "Assembling boot ROM (bootrom.S -> bootrom.bin)"
    VERBATIM
)
add_custom_target(gen_bootrom DEPENDS ${BOOTROM_BIN})

# devices.cpp #embeds bootrom.bin; make the object depend on it and silence the
# C23 #embed extension warning (we compile C++20 with clang).
set_source_files_properties(src/devices.cpp PROPERTIES
    OBJECT_DEPENDS ${BOOTROM_BIN}
    COMPILE_OPTIONS "-Wno-c23-extensions"
)

# Single-core simulation driver (no DRAM dependency)
add_executable(sim_single
    ${MIMALLOC_OBJ}
    src/single.cpp
    src/devices.cpp
)
add_dependencies(sim_single gen_bootrom)
target_compile_definitions(sim_single PRIVATE CORE_MEM_BUS_WIDTH=${CORE_MEM_BUS_WIDTH})

verilate(
    sim_single
  SOURCES ${CVFPU_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/../FPU.sv
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

# Read numPU, numMC, and per-MC size from JSON for gen_system_header.py,
# Verilator, and for passing into sim_system's now-required CLI args (used by
# BOLT profiling below).
file(READ ${SYSTEM_CONFIG_JSON} SYSTEM_CONFIG_CONTENT)
string(JSON SYSTEM_NUM_PU GET ${SYSTEM_CONFIG_CONTENT} "numPU")
string(JSON SYSTEM_NUM_MC GET ${SYSTEM_CONFIG_CONTENT} "numMC")
string(JSON SYSTEM_MC_SIZE_0 GET ${SYSTEM_CONFIG_CONTENT} "core" "memCtrlSizes" 0)

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
if(ENABLE_HARD_BACKEND)
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/hard_backend_stub.cpp "// stub for verilator linkage\n")
  add_library(hard_backend_lib STATIC
      ${CMAKE_CURRENT_BINARY_DIR}/hard_backend_stub.cpp
  )
  verilate(
    hard_backend_lib
      SOURCES ${CVFPU_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/../FPU.sv
      PREFIX sys_rtl
      TRACE_FST
      DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/sys_verilated
      INCLUDE_DIRS ${GENERATED_SYSTEM_DIR}
      VERILATOR_ARGS "-f" "${GENERATED_SYSTEM_DIR}/filelist.f" ${VERILATOR_SYSTEM_ARGS}
  )
endif()

# Soft backend: many Core instances + C++ NoC model
# Uses Core.sv from generated/system so config matches the hard backend exactly.
add_library(soft_backend_lib STATIC
    src/soft_backend.cpp
)

verilate(
  soft_backend_lib
    SOURCES ${CVFPU_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/../FPU.sv
    PREFIX soft_rtl
    TRACE_FST
    DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/soft_verilated
    INCLUDE_DIRS ${GENERATED_SYSTEM_DIR}
    VERILATOR_ARGS "-f" "${GENERATED_SYSTEM_DIR}/filelist.f" ${VERILATOR_CORE_ARGS}
)

# Unified binary: sim_system --soft / --hard / --soft --hard (cosim)
add_executable(sim_system
  ${MIMALLOC_OBJ}
  src/system.cpp
  src/devices.cpp
)
add_dependencies(sim_system gen_system_headers gen_bootrom gen_ramulator_config)
add_dependencies(soft_backend_lib gen_system_headers)
target_link_libraries(sim_system PRIVATE
  soft_backend_lib
  Ramulator::ramulator
)
target_compile_definitions(sim_system PRIVATE
  HARD_BACKEND_ENABLED=$<BOOL:${ENABLE_HARD_BACKEND}>
)

if(ENABLE_HARD_BACKEND)
  target_link_libraries(sim_system PRIVATE hard_backend_lib)
endif()

if(ENABLE_HARD_BACKEND)
  set(BOLT_SYSTEM_BACKEND_ARGS "--soft" "--hard")
else()
  set(BOLT_SYSTEM_BACKEND_ARGS "--soft")
endif()

apply_bolt(sim_system "${BOLT_MULTI_PAYLOAD}" "${BOLT_MULTI_CYCLES}"
  ${BOLT_SYSTEM_BACKEND_ARGS}
  "--pu" "${SYSTEM_NUM_PU}" "--mc" "${SYSTEM_NUM_MC}"
  "--mc-size" "${SYSTEM_MC_SIZE_0}"
  # apply_bolt appends BOLT_PAYLOAD (= dram.1) at the end, so dram.0 goes
  # here to satisfy the MC0/MC1 positional ordering.
  "${BOLT_MULTI_DRAM0}")
