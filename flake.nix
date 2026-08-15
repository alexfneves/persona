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
      lib = nixpkgs.lib;

      # ------------------------------------------------------------------
      # Backend-aware derivations. Each derivation is parameterized over the
      # enabled inference backends; the flake exposes one package per backend
      # so users build what they need, e.g.:
      #
      #   nix build .#persona            # CPU backend (default)
      #   nix build .#persona-vulkan     # Vulkan (AMD/NVIDIA via RADV/Mesa)
      #   nix build .#persona-cuda       # (future; needs ENGINE_ENABLE_CUDA)
      #
      # ggml compiles backend kernels into the static archives, so the persona
      # link line and build/runtime inputs differ per backend.
      # ------------------------------------------------------------------

      # audio.cpp composite-build flags shared by audiocpp-cli and audiocpp-lib.
      compositeFlags = { enableVulkan }:
        [
          "-G Ninja"
          "-DCMAKE_BUILD_TYPE=Release"
          "-DAUDIOCPP_MODEL_SET=custom"
          "-DAUDIOCPP_MODELS=qwen3_asr,pocket_tts"
          "-DAUDIOCPP_DEPLOYMENT_BUILD=ON"
        ]
        ++ lib.optionals enableVulkan [
          "-DENGINE_ENABLE_VULKAN=ON"
        ];

      # Per-backend inputs for the audio.cpp CMake build.
      mkAudiocppBuildInputs = { enableVulkan }:
        lib.optionals enableVulkan [ pkgs.vulkan-loader ];
      mkAudiocppNativeBuildInputs = { enableVulkan }:
        lib.optionals enableVulkan [ pkgs.vulkan-headers pkgs.shaderc pkgs.glslang ];

      # audio.cpp CLI smoke-test / benchmark build (Phase 0 derisk, extended
      # for GPU derisking). CPU-only composite unless enableVulkan.
      mkAudiocppCli = { enableVulkan ? false }:
        pkgs.stdenv.mkDerivation {
          name = if enableVulkan then "audiocpp-cli-vulkan" else "audiocpp-cli";
          src = inputs.audiocpp;
          nativeBuildInputs = [ pkgs.cmake pkgs.ninja ]
            ++ mkAudiocppNativeBuildInputs { inherit enableVulkan; };
          buildInputs = mkAudiocppBuildInputs { inherit enableVulkan; };
          cmakeFlags = compositeFlags { inherit enableVulkan; };
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
      mkAudiocppLib = { enableVulkan ? false }:
        pkgs.stdenv.mkDerivation {
          name = if enableVulkan then "audiocpp-lib-vulkan" else "audiocpp-lib";
          src = inputs.audiocpp;
          nativeBuildInputs = [ pkgs.cmake pkgs.ninja ]
            ++ mkAudiocppNativeBuildInputs { inherit enableVulkan; };
          buildInputs = mkAudiocppBuildInputs { inherit enableVulkan; };
          # Build in-tree (no separate build/ dir) so the installPhase below can
          # find the .a archives, headers, assets and specs from a single cwd.
          dontUseCmakeBuildDir = true;
          cmakeFlags = compositeFlags { inherit enableVulkan; }
            ++ [
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

      # The persona binary for a given set of enabled backends. Raw clang++
      # build (no CMake in the persona repo). T1-confirmed artifact order:
      # engine_runtime + ggml + ggml-base + ggml-cpu + sentencepiece + cjson +
      # yaml; libgomp instead of bare -fopenmp (clang 21 in nixpkgs can't find
      # -lomp). PERSONA_SPECS_DIR points at the catalog shipped by the lib.
      mkPersona = { enableVulkan ? false }:
        let
          audiocpp-lib = mkAudiocppLib { inherit enableVulkan; };
        in
        pkgs.stdenv.mkDerivation {
          name = if enableVulkan then "persona-vulkan" else "persona";
          src = ./.;
          buildInputs = [ pkgs.clang pkgs.nlohmann_json pkgs.curl pkgs.portaudio ]
            ++ lib.optionals enableVulkan [ pkgs.vulkan-loader ];
          buildPhase = ''
            clang++ -O2 -std=c++17 $(find src -name '*.cpp') -Isrc \
              -I${audiocpp-lib}/include -I${pkgs.nlohmann_json}/include \
              -I${pkgs.portaudio}/include \
              -DPERSONA_SPECS_DIR=\"${audiocpp-lib}/share/persona/model_specs\" \
              -DPERSONA_DEFAULT_BACKEND=\"${if enableVulkan then "vulkan" else "cpu"}\" \
              ${audiocpp-lib}/lib/libengine_runtime.a \
              ${audiocpp-lib}/lib/libggml.a \
              ${audiocpp-lib}/lib/libggml-base.a \
              ${audiocpp-lib}/lib/libggml-cpu.a \
              ${audiocpp-lib}/lib/libsentencepiece.a \
              ${audiocpp-lib}/lib/libcjson_vendor.a \
              ${audiocpp-lib}/lib/libyaml_vendor.a \
              -fopenmp=libgomp \
              -lportaudio \
              -lcurl \
              ${lib.optionalString enableVulkan "-lvulkan"} \
              -o persona
          '';
          installPhase = ''
            mkdir -p $out/bin
            cp persona $out/bin/
            # Bundled VAD assets (silero_vad) — resolved at runtime relative
            # to the binary (../assets/framework/models/silero_vad).
            cp -r ${audiocpp-lib}/assets $out/assets
          '';
        };
    in
    {
      packages.${system} = rec {
        # CPU backend — the default build.
        persona = mkPersona { };
        # Vulkan backend (AMD RADV / Mesa) — ggml compiles shaders into the
        # static lib; use with `persona ... --backend vulkan`.
        persona-vulkan = mkPersona { enableVulkan = true; };

        audiocpp-lib = mkAudiocppLib { };
        audiocpp-lib-vulkan = mkAudiocppLib { enableVulkan = true; };

        audiocpp-cli = mkAudiocppCli { };
        audiocpp-cli-vulkan = mkAudiocppCli { enableVulkan = true; };

        default = persona;
      };

      devShells.${system}.default = devenv.lib.mkShell {
        inherit inputs pkgs;
        modules = [
          ({ pkgs, config, ... }: {
            languages.cplusplus.enable = true;

            # The dev shell depends on the persona derivation:
            #   * packages  = the built binary is on PATH in the shell.
            #   * inputsFrom = the exact build inputs (clang, curl, portaudio,
            #     nlohmann-json, the audiocpp-lib static archives) are
            #     available for ad-hoc compilation/debugging.
            # Same store path as `nix build .#persona`, so the shell and the
            # build share the cache — building one never rebuilds the other.
            inputsFrom = [ self.packages.${system}.persona ];
            packages = [ self.packages.${system}.persona ];

            enterTest = ''
              # Persona smoke tests — `devenv test` runs this.
              # Test logic lives in tests/smoke.sh (see AGENTS.md): extend the
              # script, not the flake, when adding tests.
              bash tests/smoke.sh
            '';

            enterShell = ''
              echo "Development shell for Persona"
            '';  
          })
        ];
      };
    };
}
