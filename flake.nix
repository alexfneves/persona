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
      # audio.cpp is built by ITS OWN flake (input `audiocpp`). We consume its
      # packages directly — no manual cmake build on our side. Their
      # package.nix already exposes the knobs we need:
      #   * models = []            -> AUDIOCPP_MODEL_SET=full (all 49 families)
      #   * vulkanSupport = true   -> ENGINE_ENABLE_VULKAN=ON
      #   * rocm-gfx1151 + strixHaloOptimizations (Strix Halo ROCm variant)
      # Their package only ships the CLI binaries + model_specs, though, and
      # persona links engine_runtime + ggml statically — so we graft the
      # library artifacts (static archives, headers, bundled VAD assets) onto
      # THEIR derivation's installPhase. Everything else (build flags, source,
      # toolchain) stays theirs.
      # ------------------------------------------------------------------

      # audio.cpp's own package for a given backend ("cpu" | "vulkan").
      audioCppPackage = backend:
        inputs.audiocpp.packages.${system}.${backend};

      # Extends audio.cpp's installPhase to also ship the static archives,
      # public headers, bundled VAD assets, and model specs (the latter for
      # PERSONA_SPECS_DIR; their package already installs $out/model_specs).
      graftLibraryArtifacts = pkg:
        pkg.overrideAttrs (old: {
          installPhase = old.installPhase + ''
            # Library artifacts for persona's static link (T1-confirmed
            # archives: libengine_runtime.a + ggml + vendors; find -name '*.a'
            # keeps the set complete for any composite/backend).
            mkdir -p $out/lib $out/include $out/assets
            find . -name '*.a' -exec cp -t $out/lib/ {} +
            # Public engine headers + the ggml headers they include.
            cp -r $src/include/* $out/include/
            cp -r $src/external/ggml/include/* $out/include/
            # Bundled VAD assets (silero_vad / marblenet_vad) — resolved at
            # runtime relative to the persona binary (../assets/...).
            cp -r $src/assets/framework $out/assets/
            # PERSONA_SPECS_DIR (Decision 9 catalog). Their package installs
            # $out/model_specs; keep our layout consistent with the old
            # audiocpp-lib for the persona derivation's -D flag.
            mkdir -p $out/share/persona
            cp -r $out/model_specs $out/share/persona/model_specs
          '';
        });

      # The static-lib package persona links against, per backend.
      mkAudioCppLib = { enableVulkan ? false }:
        graftLibraryArtifacts (audioCppPackage (if enableVulkan then "vulkan" else "cpu"));

      # The persona binary for a given set of enabled backends. Raw clang++
      # build (no CMake in the persona repo). Link line: engine_runtime + ggml
      # + ggml-base + ggml-cpu + sentencepiece + cjson + yaml (+ ggml-vulkan
      # for the Vulkan variant); libgomp instead of bare -fopenmp (clang 21 in
      # nixpkgs can't find -lomp). PERSONA_SPECS_DIR points at the catalog
      # shipped by the grafted audio.cpp package.
      mkPersona = { enableVulkan ? false }:
        let
          engine = mkAudioCppLib { inherit enableVulkan; };
        in
        pkgs.stdenv.mkDerivation {
          name = if enableVulkan then "persona-vulkan" else "persona";
          src = ./.;
          buildInputs = [ pkgs.clang pkgs.nlohmann_json pkgs.curl pkgs.portaudio ]
            ++ lib.optionals enableVulkan [ pkgs.vulkan-loader ];
          buildPhase = ''
            clang++ -O2 -std=c++17 $(find src -name '*.cpp') -Isrc \
              -I${engine}/include -I${pkgs.nlohmann_json}/include \
              -I${pkgs.portaudio}/include \
              -DPERSONA_SPECS_DIR=\"${engine}/share/persona/model_specs\" \
              -DPERSONA_DEFAULT_BACKEND=\"${if enableVulkan then "vulkan" else "cpu"}\" \
              ${engine}/lib/libengine_runtime.a \
              ${engine}/lib/libggml.a \
              ${engine}/lib/libggml-base.a \
              ${engine}/lib/libggml-cpu.a \
              ${engine}/lib/libsentencepiece.a \
              ${engine}/lib/libcjson_vendor.a \
              ${engine}/lib/libyaml_vendor.a \
              ${lib.optionalString enableVulkan "${engine}/lib/libggml-vulkan.a"} \
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
            cp -r ${engine}/assets $out/assets
          '';
        };
    in
    {
      packages.${system} = rec {
        # CPU backend — the default build.
        persona = mkPersona { };
        # Vulkan backend (AMD RADV / Mesa) — ggml compiles shaders into the
        # static lib; use with `persona ... --backend vulkan` (default).
        persona-vulkan = mkPersona { enableVulkan = true; };

        # Grafted audio.cpp library packages (static archives + headers +
        # assets + model specs) — what persona links against.
        audiocpp-lib = mkAudioCppLib { };
        audiocpp-lib-vulkan = mkAudioCppLib { enableVulkan = true; };

        # audio.cpp's own CLI (their build, unmodified — reference/derisk).
        audiocpp-cli = audioCppPackage "cpu";
        audiocpp-cli-vulkan = audioCppPackage "vulkan";

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
            #     nlohmann-json, the audio.cpp static archives) are
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
