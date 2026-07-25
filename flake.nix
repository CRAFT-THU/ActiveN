{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.11";
    flake-utils.url = "github:numtide/flake-utils";
    ramulator2 = {
      url = "github:CMU-SAFARI/ramulator2/99a0e1e87a9321587492fef5b0bd6197928f8d68";
      flake = false;
    };
  };
  outputs = { self, nixpkgs, flake-utils, ramulator2 } :
    flake-utils.lib.eachDefaultSystem (
      system : let
        pkgs = import nixpkgs {
          inherit system;
          config.allowUnfree = true;
        };
        crossPkgs = import nixpkgs {
          inherit system;
          crossSystem = { system = "riscv64-none-elf"; };
        };
      in with pkgs; {
        devShells.default =
        let
          ramulator2YamlCpp = fetchFromGitHub {
            owner = "jbeder";
            repo = "yaml-cpp";
            rev = "yaml-cpp-0.9.0";
            sha256 = "sha256-+FOsPQY44h1g9tEw3O281LkiYKXdW2jnFKw+oTRkhGw=";
          };

          ramulator2Fmt = fetchFromGitHub {
            owner = "fmtlib";
            repo = "fmt";
            rev = "10.2.1";
            sha256 = "sha256-pEltGLAHLZ3xypD/Ur4dWPWJ9BGVXwqQyKcDWVmC3co=";
          };

          ramulator2InstallPatch = writeText "ramulator2-install.patch"
            (builtins.replaceStrings [ "@SP@" ] [ " " ] ''
            --- a/CMakeLists.txt
            +++ b/CMakeLists.txt
            @@ -6,6 +6,9 @@
               LANGUAGES CXX
             )
            @SP@
            +include(CMakePackageConfigHelpers)
            +include(GNUInstallDirs)
            +
             #### Prompt the build type ####
             if(NOT CMAKE_BUILD_TYPE)
               set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
            @@ -75,7 +78,10 @@
               LIBRARY_OUTPUT_DIRECTORY  ''${PROJECT_SOURCE_DIR}
             )
             # PUBLIC so consumers of the ramulator target get the include path transitively
            -target_include_directories(ramulator PUBLIC ''${CMAKE_CURRENT_SOURCE_DIR}/src)
            +target_include_directories(ramulator PUBLIC
            +  $<BUILD_INTERFACE:''${CMAKE_CURRENT_SOURCE_DIR}/src>
            +  $<INSTALL_INTERFACE:''${CMAKE_INSTALL_INCLUDEDIR}>
            +)
             target_link_libraries(
               ramulator
               PRIVATE fmt::fmt
            @@ -84,4 +90,34 @@
            @SP@
             add_subdirectory(src/ramulator)
            @SP@
            +write_basic_package_version_file(
            +  ''${CMAKE_CURRENT_BINARY_DIR}/RamulatorConfigVersion.cmake
            +  VERSION ''${PROJECT_VERSION}
            +  COMPATIBILITY SameMajorVersion
            +)
            +file(WRITE ''${CMAKE_CURRENT_BINARY_DIR}/RamulatorConfig.cmake
            +  "include(\"\''${CMAKE_CURRENT_LIST_DIR}/RamulatorTargets.cmake\")\n"
            +)
            +
            +install(TARGETS ramulator
            +  EXPORT RamulatorTargets
            +  LIBRARY DESTINATION ''${CMAKE_INSTALL_LIBDIR}
            +  ARCHIVE DESTINATION ''${CMAKE_INSTALL_LIBDIR}
            +  RUNTIME DESTINATION ''${CMAKE_INSTALL_BINDIR}
            +)
            +install(DIRECTORY ''${CMAKE_CURRENT_SOURCE_DIR}/src/ramulator
            +  DESTINATION ''${CMAKE_INSTALL_INCLUDEDIR}
            +  FILES_MATCHING PATTERN "*.h"
            +)
            +install(EXPORT RamulatorTargets
            +  FILE RamulatorTargets.cmake
            +  NAMESPACE Ramulator::
            +  DESTINATION ''${CMAKE_INSTALL_LIBDIR}/cmake/Ramulator
            +)
            +install(FILES
            +  ''${CMAKE_CURRENT_BINARY_DIR}/RamulatorConfig.cmake
            +  ''${CMAKE_CURRENT_BINARY_DIR}/RamulatorConfigVersion.cmake
            +  DESTINATION ''${CMAKE_INSTALL_LIBDIR}/cmake/Ramulator
            +)
            +
             if(RAMULATOR_PYTHON_BINDINGS)
          '');

          ramulator2Native = stdenv.mkDerivation {
            pname = "ramulator";
            version = "2.1.0";
            src = ramulator2;
            patches = [ ramulator2InstallPatch ];
            nativeBuildInputs = [ cmake ninja ];
            cmakeFlags = [
              "-DRAMULATOR_PYTHON_BINDINGS=OFF"
              "-DFETCHCONTENT_SOURCE_DIR_YAML-CPP=${ramulator2YamlCpp}"
              "-DFETCHCONTENT_SOURCE_DIR_FMT=${ramulator2Fmt}"
            ];
          };

          ramulator2Python = python3Packages.buildPythonPackage {
            pname = "ramulator";
            version = "2.1.0";
            src = ramulator2;
            pyproject = true;
            build-system = [ python3Packages.setuptools ];
            dependencies = [ python3Packages.pyyaml ];
            doCheck = false;
          };
        in mkShell {
          buildInputs = [
            (mill.override { jre = pkgs.jdk11; })
            circt cmake ninja verilator espresso
            cargo rustc
            python3 nodejs
            ramulator2Native ramulator2Python zlib
            crossPkgs.buildPackages.gcc
            llvmPackages.clang
            llvmPackages.lld
            llvmPackages.llvm
            bolt_21
            linuxPackages.perf
            mimalloc
          ];
        };
      }
    );
}
