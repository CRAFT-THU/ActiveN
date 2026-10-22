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
option(USE_ARCH_NATIVE "Optimize host code for the build machine" ON)
option(ENABLE_HARD_BACKEND "Build the Verilated full-system hard backend" ON)

if(USE_ARCH_NATIVE)
  add_compile_options(-march=native)
  message(STATUS "Host architecture optimization enabled (-march=native)")
endif()

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
function(apply_bolt TARGET CONFIGS BOLT_MAX_CYCLES)
  if(NOT USE_BOLT)
    return()
  endif()
  # Keep argv structured, and retain each profiling attempt independently.
  set(BOLT_HELPER ${CMAKE_SOURCE_DIR}/bolt_optimize.py)
  set(BOLT_CONFIG_ARGS)
  foreach(CONFIG IN LISTS CONFIGS)
    list(APPEND BOLT_CONFIG_ARGS --input ${CONFIG})
  endforeach()
  set_property(TARGET ${TARGET} APPEND PROPERTY LINK_DEPENDS ${BOLT_HELPER})
  add_custom_command(TARGET ${TARGET} POST_BUILD
    COMMENT "BOLT: optimizing ${TARGET}"
    COMMAND ${Python3_EXECUTABLE} -B ${BOLT_HELPER}
            --llvm-bolt ${LLVM_BOLT} --binary $<TARGET_FILE:${TARGET}>
            --evidence-root ${CMAKE_BINARY_DIR}/bolt/${TARGET}
            --kind ${TARGET} --max-cycles ${BOLT_MAX_CYCLES}
            ${BOLT_CONFIG_ARGS}
            --input ${CMAKE_BINARY_DIR}/CMakeCache.txt
            --input ${CMAKE_SOURCE_DIR}/CMakeLists.txt
            -- ${ARGN}
    VERBATIM
  )
endfunction()



include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
find_package(verilator HINTS $ENV{VERILATOR_ROOT})
find_package(Ramulator 2.1 CONFIG REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(RAMULATOR_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ramulator-config)
set(RAMULATOR_CONFIG_PY ${RAMULATOR_CONFIG_DIR}/hbm1-8ch.py)
set(RAMULATOR_CONFIG_HBM2_PY ${RAMULATOR_CONFIG_DIR}/hbm2-1ch.py)
set(RAMULATOR_CONFIG_HBM2E_PY ${RAMULATOR_CONFIG_DIR}/hbm2e-1ch.py)
set(RAMULATOR_CONFIG_HBM3_PY ${RAMULATOR_CONFIG_DIR}/hbm3-1ch.py)
set(RAMULATOR_CONFIG_YAML ${CMAKE_CURRENT_BINARY_DIR}/ramulator.yaml)
set(RAMULATOR_CONFIG_HBM2_YAML ${CMAKE_CURRENT_BINARY_DIR}/ramulator-hbm2-1ch.yaml)
set(RAMULATOR_CONFIG_HBM2E_YAML ${CMAKE_CURRENT_BINARY_DIR}/ramulator-hbm2e-1ch.yaml)
set(RAMULATOR_CONFIG_HBM3_YAML ${CMAKE_CURRENT_BINARY_DIR}/ramulator-hbm3-1ch.yaml)
add_custom_command(
    OUTPUT ${RAMULATOR_CONFIG_YAML}
    COMMAND ${Python3_EXECUTABLE} -m ramulator export
            ${RAMULATOR_CONFIG_PY} -o ${RAMULATOR_CONFIG_YAML}
    DEPENDS ${RAMULATOR_CONFIG_PY}
    COMMENT "Exporting ActiveN Ramulator configuration"
    VERBATIM
)
add_custom_command(
    OUTPUT ${RAMULATOR_CONFIG_HBM2_YAML}
    COMMAND ${Python3_EXECUTABLE} -m ramulator export
            ${RAMULATOR_CONFIG_HBM2_PY} -o ${RAMULATOR_CONFIG_HBM2_YAML}
    DEPENDS ${RAMULATOR_CONFIG_HBM2_PY}
    COMMENT "Exporting one-channel HBM2 Ramulator configuration"
    VERBATIM
)
add_custom_command(
    OUTPUT ${RAMULATOR_CONFIG_HBM2E_YAML}
    COMMAND ${Python3_EXECUTABLE} -B -m ramulator export
            ${RAMULATOR_CONFIG_HBM2E_PY} -o ${RAMULATOR_CONFIG_HBM2E_YAML}
    DEPENDS ${RAMULATOR_CONFIG_HBM2E_PY}
    COMMENT "Exporting the 1.6-GHz HBM2-derived extension configuration"
    VERBATIM
)
add_custom_command(
    OUTPUT ${RAMULATOR_CONFIG_HBM3_YAML}
    COMMAND ${Python3_EXECUTABLE} -B -m ramulator export
            ${RAMULATOR_CONFIG_HBM3_PY} -o ${RAMULATOR_CONFIG_HBM3_YAML}
    DEPENDS ${RAMULATOR_CONFIG_HBM3_PY}
    COMMENT "Exporting one-channel HBM3 Ramulator configuration"
    VERBATIM
)
add_custom_target(gen_ramulator_config DEPENDS
    ${RAMULATOR_CONFIG_YAML}
    ${RAMULATOR_CONFIG_HBM2_YAML}
    ${RAMULATOR_CONFIG_HBM2E_YAML}
    ${RAMULATOR_CONFIG_HBM3_YAML}
)

add_executable(test_hbm2e_timing EXCLUDE_FROM_ALL tests/hbm2e_timing.cpp)
add_executable(test_idle_delay EXCLUDE_FROM_ALL tests/idle_delay.cpp)
add_custom_target(check_idle_delay
    COMMAND $<TARGET_FILE:test_idle_delay>
    DEPENDS test_idle_delay
    VERBATIM
)
target_link_libraries(test_hbm2e_timing PRIVATE Ramulator::ramulator)
add_custom_target(check_hbm2e_timing
    COMMAND $<TARGET_FILE:test_hbm2e_timing> ${RAMULATOR_CONFIG_HBM2E_YAML}
    DEPENDS test_hbm2e_timing gen_ramulator_config
    VERBATIM
)

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
set(GENERATED_CORE_DIR ${CMAKE_SOURCE_DIR}/../generated/core CACHE PATH "Core RTL directory for sim_single and soft-only sim_system")
set(GENERATED_SYSTEM_DIR ${CMAKE_SOURCE_DIR}/../generated/system CACHE PATH "System RTL directory")

set(CORE_CONFIG_JSON ${GENERATED_CORE_DIR}/Core.config.json)
if(NOT EXISTS ${CORE_CONFIG_JSON})
  message(FATAL_ERROR "Core.config.json not found at ${CORE_CONFIG_JSON}. Run Mill elaboration first.")
endif()
file(READ ${CORE_CONFIG_JSON} CORE_CONFIG_CONTENT)
string(JSON CORE_MEM_BUS_WIDTH GET ${CORE_CONFIG_CONTENT} "memBusWidth")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CORE_CONFIG_JSON})

# A cosim build takes both backend cores from the same System elaboration.
# A soft-only build needs only the selected Core export.
if(ENABLE_HARD_BACKEND)
  set(SYSTEM_CONFIG_JSON ${GENERATED_SYSTEM_DIR}/System.config.json)
  if(NOT EXISTS ${SYSTEM_CONFIG_JSON})
    message(FATAL_ERROR "System.config.json not found at ${SYSTEM_CONFIG_JSON}. Run system elaboration first.")
  endif()
  file(READ ${SYSTEM_CONFIG_JSON} SYSTEM_CONFIG_CONTENT)
  string(JSON SYSTEM_NUM_PU GET ${SYSTEM_CONFIG_CONTENT} "numPU")
  string(JSON SYSTEM_NUM_MC GET ${SYSTEM_CONFIG_CONTENT} "numMC")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SYSTEM_CONFIG_JSON})
  set(SYSTEM_CORE_DIR ${GENERATED_SYSTEM_DIR})
else()
  set(SYSTEM_CORE_DIR ${GENERATED_CORE_DIR})
endif()
set(SYSTEM_CORE_CONFIG_JSON ${SYSTEM_CORE_DIR}/Core.config.json)
if(NOT EXISTS ${SYSTEM_CORE_CONFIG_JSON})
  message(FATAL_ERROR "Core.config.json not found at ${SYSTEM_CORE_CONFIG_JSON}. Regenerate the selected RTL export.")
endif()
file(READ ${SYSTEM_CORE_CONFIG_JSON} SYSTEM_CORE_CONFIG)
string(JSON SYSTEM_CORE_MC_COUNT LENGTH ${SYSTEM_CORE_CONFIG} "memCtrlSizes")
if(SYSTEM_CORE_MC_COUNT LESS 1)
  message(FATAL_ERROR "Core.config.json must provide at least one MC capacity")
endif()
string(JSON SYSTEM_MC_SIZE_0 GET ${SYSTEM_CORE_CONFIG} "memCtrlSizes" 0)
if(ENABLE_HARD_BACKEND AND NOT SYSTEM_CORE_MC_COUNT EQUAL SYSTEM_NUM_MC)
  message(FATAL_ERROR "Core.config.json memCtrlSizes count must match System.config.json numMC")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SYSTEM_CORE_CONFIG_JSON})

# Geometry is taken from the same elaboration as each Verilated model.
function(icache_dpi_args OUT CONFIG)
  string(JSON LINES GET "${CONFIG}" "iCacheLines")
  string(JSON BLOCK GET "${CONFIG}" "iCacheBlockSize")
  string(JSON ASSOC GET "${CONFIG}" "iCacheAssoc")
  set(${OUT} -DAN_ICACHE_LINES=${LINES} -DAN_ICACHE_BLOCK_SIZE=${BLOCK}
             -DAN_ICACHE_ASSOC=${ASSOC} PARENT_SCOPE)
endfunction()
icache_dpi_args(CORE_ICACHE_ARGS "${CORE_CONFIG_CONTENT}")
icache_dpi_args(SYSTEM_ICACHE_ARGS "${SYSTEM_CORE_CONFIG}")
set(ICACHE_DPI_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/icache_dpi.sv)

function(spm_dpi_args OUT CONFIG)
  string(JSON WIDTH GET "${CONFIG}" "memBusWidth")
  math(EXPR LINE_BYTES "${WIDTH} / 8")
  set(${OUT} -DAN_SPM_LINE_BYTES=${LINE_BYTES} PARENT_SCOPE)
endfunction()
spm_dpi_args(CORE_SPM_ARGS "${CORE_CONFIG_CONTENT}")
spm_dpi_args(SYSTEM_SPM_ARGS "${SYSTEM_CORE_CONFIG}")
set(SPM_DPI_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/spm_dpi.sv)

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
# SNN workload: one compatible DRAM image per profiling MC.
set(BOLT_SYSTEM_IMAGE_DIR "${CMAKE_SOURCE_DIR}/../workloads/data/fixtures/snn_shuffle" CACHE PATH
    "Directory containing compatible SNN BOLT profiling images")
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
  SOURCES ${CVFPU_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/../FPU.sv ${ICACHE_DPI_SOURCE} ${SPM_DPI_SOURCE}
    PREFIX rtl
    TRACE_FST
    DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/verilated
    INCLUDE_DIRS ${GENERATED_CORE_DIR}
    VERILATOR_ARGS "-f" "${GENERATED_CORE_DIR}/filelist.f" ${VERILATOR_CORE_ARGS} ${CORE_ICACHE_ARGS} ${CORE_SPM_ARGS}
)
apply_bolt(sim_single ${CORE_CONFIG_JSON} ${BOLT_SINGLE_CYCLES} ${BOLT_SINGLE_PAYLOAD})

# Shared Core geometry has no fixed runtime PU/MC topology.
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/core_config.h
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/gen_core_header.py
            --config ${SYSTEM_CORE_CONFIG_JSON}
            --output ${CMAKE_CURRENT_BINARY_DIR}/core_config.h
    DEPENDS ${SYSTEM_CORE_CONFIG_JSON} ${CMAKE_CURRENT_SOURCE_DIR}/gen_core_header.py
    COMMENT "Generating core_config.h from Core.config.json"
    VERBATIM
)
add_custom_target(gen_core_header DEPENDS
    ${CMAKE_CURRENT_BINARY_DIR}/core_config.h
)

# Hard backend: Verilated System RTL
if(ENABLE_HARD_BACKEND)
  add_custom_command(
      OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/hard_backend.h
      COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/gen_system_header.py
              --config ${SYSTEM_CONFIG_JSON}
              --output ${CMAKE_CURRENT_BINARY_DIR}/hard_backend.h
      DEPENDS ${SYSTEM_CONFIG_JSON} ${CMAKE_CURRENT_SOURCE_DIR}/gen_system_header.py
      COMMENT "Generating hard_backend.h from System.config.json"
      VERBATIM
  )
  add_custom_target(gen_hard_backend_header DEPENDS
      ${CMAKE_CURRENT_BINARY_DIR}/hard_backend.h
  )
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/hard_backend_stub.cpp "// stub for verilator linkage\n")
  add_library(hard_backend_lib STATIC
      ${CMAKE_CURRENT_BINARY_DIR}/hard_backend_stub.cpp
  )
  verilate(
    hard_backend_lib
      SOURCES ${CVFPU_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/../FPU.sv ${ICACHE_DPI_SOURCE} ${SPM_DPI_SOURCE}
      PREFIX sys_rtl
      TRACE_FST
      DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/sys_verilated
      INCLUDE_DIRS ${GENERATED_SYSTEM_DIR}
      VERILATOR_ARGS "-f" "${GENERATED_SYSTEM_DIR}/filelist.f" ${VERILATOR_SYSTEM_ARGS} ${SYSTEM_ICACHE_ARGS} ${SYSTEM_SPM_ARGS}
  )
endif()

# Soft backend: many Core instances + C++ NoC model
# Uses the selected Core export, or the matching System export for cosim.
add_library(soft_backend_lib STATIC
    src/soft_backend.cpp
)

verilate(
  soft_backend_lib
    SOURCES ${CVFPU_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/../FPU.sv ${ICACHE_DPI_SOURCE} ${SPM_DPI_SOURCE}
    PREFIX soft_rtl
    TRACE_FST
    DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/soft_verilated
    INCLUDE_DIRS ${SYSTEM_CORE_DIR}
    VERILATOR_ARGS "-f" "${SYSTEM_CORE_DIR}/filelist.f" ${VERILATOR_CORE_ARGS} ${SYSTEM_ICACHE_ARGS} ${SYSTEM_SPM_ARGS}
)

# Unified binary: sim_system --soft / --hard / --soft --hard (cosim)
add_executable(sim_system
  ${MIMALLOC_OBJ}
  src/system.cpp
  src/devices.cpp
)
add_dependencies(sim_system gen_core_header gen_bootrom gen_ramulator_config)
add_dependencies(soft_backend_lib gen_core_header)
target_link_libraries(sim_system PRIVATE
  soft_backend_lib
  Ramulator::ramulator
)
target_compile_definitions(sim_system PRIVATE
  HARD_BACKEND_ENABLED=$<BOOL:${ENABLE_HARD_BACKEND}>
)

if(ENABLE_HARD_BACKEND)
  target_link_libraries(sim_system PRIVATE hard_backend_lib)
  add_dependencies(sim_system gen_hard_backend_header)
endif()

# These values are derived from backend mode, not independent cache options.
unset(BOLT_SYSTEM_NUM_PU CACHE)
unset(BOLT_SYSTEM_NUM_MC CACHE)
set(BOLT_SYSTEM_NUM_PU 64)
set(BOLT_SYSTEM_NUM_MC 2)
set(BOLT_SYSTEM_CONFIGS ${SYSTEM_CORE_CONFIG_JSON})
if(ENABLE_HARD_BACKEND)
  set(BOLT_SYSTEM_BACKEND_ARGS "--soft" "--hard")
  set(BOLT_SYSTEM_NUM_PU ${SYSTEM_NUM_PU})
  set(BOLT_SYSTEM_NUM_MC ${SYSTEM_NUM_MC})
  list(APPEND BOLT_SYSTEM_CONFIGS ${SYSTEM_CONFIG_JSON})
else()
  set(BOLT_SYSTEM_BACKEND_ARGS "--soft")
endif()

if(USE_BOLT)
  if(NOT BOLT_SYSTEM_NUM_PU MATCHES "^[1-9][0-9]*$" OR
     NOT BOLT_SYSTEM_NUM_MC MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "BOLT training PU and MC counts must be positive integers")
  endif()
  math(EXPR BOLT_MC_CAPACITY "${SYSTEM_MC_SIZE_0}")
  if(ENABLE_HARD_BACKEND)
    math(EXPR CORE_LAST_MC "${SYSTEM_CORE_MC_COUNT} - 1")
    foreach(MC RANGE ${CORE_LAST_MC})
      string(JSON MC_CAPACITY GET ${SYSTEM_CORE_CONFIG} "memCtrlSizes" ${MC})
      math(EXPR MC_CAPACITY "${MC_CAPACITY}")
      if(NOT MC_CAPACITY EQUAL BOLT_MC_CAPACITY)
        message(FATAL_ERROR "BOLT's --mc-size argument requires uniform hardware MC capacities")
      endif()
    endforeach()
  endif()
  math(EXPR BOLT_LAST_MC "${BOLT_SYSTEM_NUM_MC} - 1")
  set(BOLT_SYSTEM_IMAGES)
  foreach(MC RANGE ${BOLT_LAST_MC})
    set(BOLT_IMAGE ${BOLT_SYSTEM_IMAGE_DIR}/dram.${MC})
    if(NOT EXISTS ${BOLT_IMAGE})
      message(FATAL_ERROR "Missing BOLT training image: ${BOLT_IMAGE}")
    endif()
    file(SIZE ${BOLT_IMAGE} BOLT_IMAGE_BYTES)
    if(BOLT_IMAGE_BYTES GREATER BOLT_MC_CAPACITY)
      message(FATAL_ERROR "BOLT image ${BOLT_IMAGE} exceeds compiled per-MC capacity")
    endif()
    list(APPEND BOLT_SYSTEM_IMAGES ${BOLT_IMAGE})
  endforeach()
endif()

apply_bolt(sim_system "${BOLT_SYSTEM_CONFIGS}" ${BOLT_MULTI_CYCLES}
  ${BOLT_SYSTEM_BACKEND_ARGS}
  "--soft-threads" "8"
  "--pu" "${BOLT_SYSTEM_NUM_PU}" "--mc" "${BOLT_SYSTEM_NUM_MC}"
  "--mc-size" "${SYSTEM_MC_SIZE_0}"
  ${BOLT_SYSTEM_IMAGES})
