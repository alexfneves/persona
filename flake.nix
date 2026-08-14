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

        # Reusable static-lib package that the persona binary links against
        # (T2). Same composite build as audiocpp-cli, but ships everything a
        # consumer needs: all static archives, public headers, bundled VAD
        # assets, and the model catalog (Decision 9).
        audiocpp-lib = pkgs.stdenv.mkDerivation {
          name = "audiocpp-lib";
          src = inputs.audiocpp;
          nativeBuildInputs = [ pkgs.cmake pkgs.ninja ];
          # Build in-tree (no separate build/ dir) so the installPhase below can
          # find the .a archives, headers, assets and specs from a single cwd.
          dontUseCmakeBuildDir = true;
          cmakeFlags = [
            "-G Ninja"
            "-DCMAKE_BUILD_TYPE=Release"
            "-DAUDIOCPP_MODEL_SET=custom"
            "-DAUDIOCPP_MODELS=qwen3_asr,pocket_tts"
            "-DAUDIOCPP_DEPLOYMENT_BUILD=ON"
            "-DENGINE_BUILD_EXAMPLES=OFF"
            "-DENGINE_BUILD_TESTS=OFF"
          ];
          installPhase = ''
            runHook preInstall
            mkdir -p $out/lib $out/include $out/assets $out/share/persona/model_specs
            # All static archives from the composite build: libengine_runtime.a
            # (top), ggml/src/libggml{,-base,-cpu}.a,
            # external/sentencepiece/src/libsentencepiece.a,
            # libcjson_vendor.a, libyaml_vendor.a. Copy every .a so the persona
            # link line never misses a transitive archive.
            find . -name '*.a' -exec cp -t $out/lib/ {} +
            cp -r include/* $out/include/
            # Public engine headers pull in ggml.h/ggml-backend.h/ggml-alloc.h
            # (external/ggml/include) — ship them so the lib is self-contained.
            cp -r external/ggml/include/* $out/include/
            # Preserve the framework/models/ layout (cp of a subdir would
            # collapse it to $out/assets/models).
            cp -r assets/framework $out/assets/
            # Decision 9: the searchable model catalog (47 spec JSONs).
            cp model_specs/*.json $out/share/persona/model_specs/
            runHook postInstall
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
