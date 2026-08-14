{
  inputs = {
    nixpkgs.url = "github:cachix/devenv-nixpkgs/rolling";
    devenv.url = "github:cachix/devenv";
  };

  nixConfig = {
    extra-trusted-public-keys = "devenv.cachix.org-1:w1cLUi8dv3hnoSPGAuibQv+f9TZLr6cv/Hm9XgU50cw=";
    extra-substituters = "https://devenv.cachix.org";
  };

  outputs = { self, nixpkgs, devenv, ... } @ inputs:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
    in
    {
      packages.${system} = rec {
        persona = pkgs.stdenv.mkDerivation {
          name = "persona";
          src = ./.;
          buildInputs = [ pkgs.clang ];
          installPhase = ''
            mkdir -p $out/bin
            clang++ -O2 -std=c++17 src/persona.cpp -o $out/bin/persona
          '';
        };

        default = persona;
      };

      devShells.${system}.default = devenv.lib.mkShell {
        inherit inputs pkgs;
        modules = [
          ({ pkgs, config, ... }: {
            languages.cplusplus.enable = true;

            enterTest = ''
            '';

            enterShell = ''
              echo "Development shell for Persona"
            '';  
          })
        ];
      };
    };
}
