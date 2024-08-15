{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/24.05";
    flake-utils.url = "github:numtide/flake-utils";
  };
  outputs = { self, nixpkgs, flake-utils } :
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
        let dramsim3 = stdenv.mkDerivation {
          name = "dramsim3";
          src = fetchFromGitHub {
            owner = "CircuitCoder";
            repo = "dramsim3";
            rev = "0480d75ddee1eaf3b8dd5e1f8abd0b34cc2ab9ba";
            sha256 = "sha256-DJZ2QdrWHf7PEKju8Q5huoNBLA+E4JI3wcgaVsdzag0=";
          };
          nativeBuildInputs = [ cmake ninja ];
        }; in mkShell {
          buildInputs = [
            (mill.override { jre = pkgs.jdk8; })
            circt cmake ninja verilator espresso
            cargo rustc
            python3 nodejs
            dramsim3 zlib
            crossPkgs.buildPackages.gcc
          ];
        };
      }
    );
}
