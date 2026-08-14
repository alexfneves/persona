{
  inputs = {
    nixpkgs.url = "github:cachix/devenv-nixpkgs/rolling";
    devenv.url = "github:cachix/devenv";
    audiocpp.url = "github:0xShug0/audio.cpp/release-0.6";
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

        # audio.cpp CLI smoke-test build (Phase 0 derisk). CPU-only composite
        # build: qwen3_asr + pocket_tts + bundled silero_vad/marblenet_vad.
        # AUDIOCPP_DEPLOYMENT_BUILD=ON embeds model_specs/*.json into the runtime.
        audiocpp-cli = pkgs.stdenv.mkDerivation {
          name = "audiocpp-cli";
          src = inputs.audiocpp;
          nativeBuildInputs = [ pkgs.cmake pkgs.ninja ];
          cmakeFlags = [
            "-G Ninja"
            "-DCMAKE_BUILD_TYPE=Release"
            "-DAUDIOCPP_MODEL_SET=custom"
            "-DAUDIOCPP_MODELS=qwen3_asr,pocket_tts"
            "-DAUDIOCPP_DEPLOYMENT_BUILD=ON"
          ];
          buildTargets = [ "audiocpp_cli" ];
          installPhase = ''
            mkdir -p $out/bin
            cp bin/audiocpp_cli $out/bin/
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
