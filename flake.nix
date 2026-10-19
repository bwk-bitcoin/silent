{
  description = "Silent - Privacy-focused desktop Bitcoin wallet using Silent Payments";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.05";

    qt_static = {
      url = "github:pythcoiner/qt_static/6.6.3";
      flake = false;  # Use as source only — its flake.nix requires impure Qt source
    };

    qt_src = {
      url = "github:qt/qtbase/v6.6.3";
      flake = false;
    };

    qt_svg_src = {
      url = "github:qt/qtsvg/v6.6.3";
      flake = false;
    };

    bwk = {
      url = "github:pythcoiner/bwk/signing_rework";
      flake = false;
    };

    spdk = {
      url = "github:pythcoiner/spdk/blindbit_backend_non_async";
      flake = false;
    };

    qontrol = {
      url = "github:pythcoiner/qontrol/dfc65d1";
      flake = false;
    };

    rust-overlay = {
      url = "github:oxalica/rust-overlay";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = { self, nixpkgs, qt_static, qt_src, qt_svg_src, bwk, spdk, qontrol, rust-overlay }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs {
        inherit system;
        overlays = [ rust-overlay.overlays.default ];
        config = {
          allowUnfree = true;           # Required for Xcode SDK
          allowUnsupportedSystem = true; # Required for darwin cross-tools on Linux
        };
      };

      qtVersion = "6.6.3";

      # Build static Qt6 for Linux using qt_static's nix/linux.nix
      linuxQt6Base = pkgs.callPackage "${qt_static}/nix/linux.nix" {
        qt6Source = qt_src;
        inherit qtVersion;
      };

      linuxQt6Svg = (pkgs.callPackage "${qt_static}/nix/qtsvg-linux.nix" {
        qtSvgSource = qt_svg_src;
        qt6Linux = linuxQt6Base;
        inherit qtVersion;
      }).overrideAttrs (old: {
        buildInputs = old.buildInputs ++ (with pkgs; [
          zlib pcre2 double-conversion libb2 openssl libpng libjpeg sqlite
          glib fontconfig freetype harfbuzz dbus at-spi2-core libinput mtdev systemd
          xorg.libX11 xorg.libXext xorg.libXrender xorg.libXi xorg.libXcursor
          xorg.libXrandr xorg.libXinerama xorg.libXfixes xorg.libXcomposite
          xorg.libXdamage xorg.libxcb xorg.xcbutil xorg.xcbutilwm xorg.xcbutilimage
          xorg.xcbutilkeysyms xorg.xcbutilrenderutil xorg.xcbutilcursor
          xorg.libSM xorg.libICE libxkbcommon wayland wayland-protocols
          mesa libGL vulkan-headers vulkan-loader libdrm xorg.libxshmfence
        ]);
      });

      linuxQt6 = pkgs.callPackage "${qt_static}/nix/combine.nix" {
        qtbase = linuxQt6Base;
        qtsvg = linuxQt6Svg;
      };

      # Vendor Cargo dependencies (handles git deps via outputHashes)
      cargoVendorDir = pkgs.rustPlatform.importCargoLock {
        lockFile = ./silent/Cargo.lock;
        outputHashes = {
          "blindbitd-0.0.1" = "sha256-XfO7P9uVLbw8mpiqLZbUoKw8XsxqM1MyeBuq6TKeZ24=";
          "bwk-sp-0.1.0" = "sha256-QgeR4t/i95RZjSb7wl+saCYZ95eLRf7YWIFMDGbAhbo=";
          "electrsd-0.29.0" = "sha256-kpma4FwkvZxjvVlN/9TnpmWpABkN3iNgQZUtFNp65IM=";
        };
      };

      # Rust toolchain with cross-compilation targets
      rustToolchain = pkgs.rust-bin.stable.latest.default.override {
        targets = [
          "x86_64-unknown-linux-gnu"
          "x86_64-pc-windows-gnu"
          "aarch64-apple-darwin"
          "x86_64-apple-darwin"
        ];
      };

      # Filtered source containing only files that affect dependency compilation.
      # Changes to account.rs/config.rs won't invalidate this derivation.
      depsOnlySource = pkgs.lib.fileset.toSource {
        root = ./.;
        fileset = pkgs.lib.fileset.unions [
          ./silent/Cargo.toml
          ./silent/Cargo.lock
          ./silent/build.rs
          ./silent/src/lib.rs
        ];
      };

      # Shared cargo config written during both deps and final builds
      cargoConfig = ''
        mkdir -p /build/src/.cargo
        cat > /build/src/.cargo/config.toml <<CARGO_EOF
[source.crates-io]
replace-with = "vendored-sources"

[source."git+https://github.com/pythcoiner/bwk.git?rev=093524e0a4bdeebacfb4fdf3e8c11e7227def06b"]
git = "https://github.com/pythcoiner/bwk.git"
rev = "093524e0a4bdeebacfb4fdf3e8c11e7227def06b"
replace-with = "vendored-sources"

[source."git+https://github.com/pythcoiner/blindbitd.git"]
git = "https://github.com/pythcoiner/blindbitd.git"
replace-with = "vendored-sources"

[source."git+https://github.com/pythcoiner/electrsd.git?branch=buffered_logs"]
git = "https://github.com/pythcoiner/electrsd.git"
branch = "buffered_logs"
replace-with = "vendored-sources"

[source.vendored-sources]
directory = "${cargoVendorDir}"
CARGO_EOF
      '';

      # Shared source layout: bwk/spdk/silent as siblings under /build/src/
      sourceLayout = silentSrc: ''
        mkdir -p /build/src/{bwk,spdk,silent}
        cp -r ${bwk}/. /build/src/bwk/
        cp -r ${spdk}/. /build/src/spdk/
        cp -r ${silentSrc}/. /build/src/silent/
        chmod -R u+w /build/src
      '';

      # Phase 1: Build all dependency crates (cached when Cargo.lock/Cargo.toml/build.rs/lib.rs unchanged).
      # Uses a stub for account.rs/config.rs so only the CXX bridge + deps compile.
      buildRustDeps = { rustTarget ? null
                      , extraNativeBuildInputs ? []
                      , extraBuildInputs ? []
                      , preBuildSetup ? ""
                      }:
        let
          targetArg = if rustTarget != null then "--target ${rustTarget}" else "";
        in
        pkgs.stdenv.mkDerivation {
          pname = "silent-rust-deps";
          version = "0.1.0";

          dontUnpack = true;
          dontConfigure = true;

          nativeBuildInputs = [
            rustToolchain
            pkgs.pkg-config
            pkgs.perl
            # mozjpeg-sys (behind nokhwa's Linux-only "decoding" feature) builds
            # with its nasm_simd feature active; see the mozjpeg-sys/nasm_simd
            # cargo tree entry checked when that feature was enabled.
            pkgs.nasm
          ] ++ extraNativeBuildInputs;

          buildInputs = (with pkgs; [
            openssl
            openssl.dev
            systemd.dev
          ]) ++ extraBuildInputs;

          buildPhase = ''
            export HOME=$TMPDIR

            ${sourceLayout depsOnlySource}
            ${cargoConfig}

            # Create stub modules so deps compile without the real implementation
            mkdir -p /build/src/silent/silent/src
            touch /build/src/silent/silent/src/account.rs
            touch /build/src/silent/silent/src/config.rs

            ${preBuildSetup}

            cd /build/src/silent/silent
            cargo build --release ${targetArg} || true
          '';

          installPhase = ''
            cd /build/src/silent/silent
            # Save the target directory with all compiled deps
            mkdir -p $out
            cp -r target $out/target
            # Remove only silent crate fingerprints so it recompiles with real source
            find $out/target -name "libsilent*" -delete
            find $out/target -name "silent-*" -path "*/fingerprint/*" -exec rm -rf {} + 2>/dev/null || true
            find $out/target -name "silent-*" -path "*/deps/*" -delete 2>/dev/null || true
            find $out/target -name "silent-*" -path "*/build/*" -exec rm -rf {} + 2>/dev/null || true
          '';

          dontFixup = true;
        };

      # Phase 2: Build the Rust static library + CXX bridge headers.
      # Reuses cached dependency artifacts from buildRustDeps.
      buildRustLib = { rustTarget ? null
                     , extraNativeBuildInputs ? []
                     , extraBuildInputs ? []
                     , preBuildSetup ? ""
                     }:
        let
          targetArg = if rustTarget != null then "--target ${rustTarget}" else "";
          releaseDir = if rustTarget != null
                       then "target/${rustTarget}/release"
                       else "target/release";
          deps = buildRustDeps {
            inherit rustTarget extraNativeBuildInputs extraBuildInputs preBuildSetup;
          };
        in
        pkgs.stdenv.mkDerivation {
          pname = "silent-rust";
          version = "0.1.0";

          dontUnpack = true;
          dontConfigure = true;

          nativeBuildInputs = [
            rustToolchain
            pkgs.pkg-config
            pkgs.perl
            # mozjpeg-sys (behind nokhwa's Linux-only "decoding" feature) builds
            # with its nasm_simd feature active; see the mozjpeg-sys/nasm_simd
            # cargo tree entry checked when that feature was enabled.
            pkgs.nasm
          ] ++ extraNativeBuildInputs;

          buildInputs = (with pkgs; [
            openssl
            openssl.dev
            systemd.dev
          ]) ++ extraBuildInputs;

          buildPhase = ''
            export HOME=$TMPDIR

            ${sourceLayout self}
            ${cargoConfig}

            # Restore cached dependency artifacts
            cp -r ${deps}/target /build/src/silent/silent/target
            chmod -R u+w /build/src/silent/silent/target

            ${preBuildSetup}

            cd /build/src/silent/silent
            cargo build --release ${targetArg}
          '';

          installPhase = ''
            cd /build/src/silent/silent
            mkdir -p $out/lib $out/include
            cp ${releaseDir}/libsilent.a $out/lib/

            # CXX bridge headers may be under target/cxxbridge/ or target/<triple>/cxxbridge/
            if [ -f target/cxxbridge/silent/src/lib.rs.h ]; then
              cp target/cxxbridge/silent/src/lib.rs.h $out/include/silent.h
              cp target/cxxbridge/rust/cxx.h $out/include/cxx.h
            else
              find target -name 'lib.rs.h' -path '*/cxxbridge/silent/src/*' | head -1 | xargs -I{} cp {} $out/include/silent.h
              find target -name 'cxx.h' -path '*/cxxbridge/rust/*' | head -1 | xargs -I{} cp {} $out/include/cxx.h
            fi
          '';

          dontFixup = true;
        };

      # Build the C++ GUI against pre-built Rust lib and static Qt6.
      # Static Qt6 requires all its dependencies available at link time.
      buildGui = { rustLib, qt6Static
                 , stdenvOverride ? pkgs.stdenv
                 , extraCmakeFlags ? []
                 , guiBuildInputs ? linuxGuiBuildInputs
                 , postUnpackExtra ? ""
                 , preConfigureExtra ? ""
                 }:
        stdenvOverride.mkDerivation {
          pname = "silent";
          version = "0.1.0";

          dontWrapQtApps = true;

          src = self;

          nativeBuildInputs = with pkgs; [
            cmake
            ninja
            pkg-config
            python3
            qt6.qttools
          ];

          buildInputs = guiBuildInputs;

          postUnpack = ''
            cd $sourceRoot

            # Remove the RunBeforeBuild target (Rust is already built by Nix)
            sed -i '/add_custom_target(/,/)/d' CMakeLists.txt
            sed -i '/add_dependencies.*RunBeforeBuild/d' CMakeLists.txt

            # Place pre-built Rust artifacts
            mkdir -p lib/include
            cp ${rustLib}/lib/libsilent.a lib/
            cp ${rustLib}/include/silent.h lib/include/
            cp ${rustLib}/include/cxx.h lib/include/

            # Replace lib/qontrol with the flake input version
            rm -rf lib/qontrol
            cp -r ${qontrol} lib/qontrol
            chmod -R u+w lib/qontrol

            # Ensure Qontrol umbrella header exposes common.h (for qontrol::UNIQUE etc.)
            if ! grep -q 'common.h' lib/qontrol/include/Qontrol; then
              echo '#include "../src/common.h" // IWYU pragma: keep' >> lib/qontrol/include/Qontrol
            fi

            # Remove qontrol tests (may require Qt6Test which isn't always available)
            rm -rf lib/qontrol/tests
            sed -i '/add_subdirectory(tests)/d' lib/qontrol/CMakeLists.txt

            ${postUnpackExtra}

            cd ..
          '';

          preConfigure = preConfigureExtra;

          cmakeFlags = [
            "-DCMAKE_PREFIX_PATH=${qt6Static}"
          ] ++ extraCmakeFlags;

          installPhase = ''
            mkdir -p $out/bin
            if [ -f silent ]; then
              cp silent $out/bin/
            elif [ -f silent.exe ]; then
              cp silent.exe $out/bin/
            elif [ -f silent.app/Contents/MacOS/silent ]; then
              cp -r silent.app $out/
              ln -s $out/silent.app/Contents/MacOS/silent $out/bin/silent
            else
              echo "ERROR: silent binary not found"
              find . -name "silent*" -type f || true
              exit 1
            fi

            # Windows host import library: plugin DLLs link against this to resolve
            # the host ABI (interfaces + qontrol + the Qt surface the host uses).
            implib="$(find . -name libsilent_host_exports.a -print -quit || true)"
            echo "host import library: ''${implib:-<not found>}"
            if [ -n "$implib" ]; then
              install -D "$implib" $out/lib/libsilent_host_exports.a
            fi
          '';
        };

      # Linux GUI dependencies (static Qt6 transitive deps)
      linuxGuiBuildInputs = with pkgs; [
        zlib pcre2 double-conversion libb2 openssl libpng libjpeg sqlite
        glib fontconfig freetype harfbuzz dbus at-spi2-core libinput mtdev systemd
        xorg.libX11 xorg.libXext xorg.libXrender xorg.libXi xorg.libXcursor
        xorg.libXrandr xorg.libXinerama xorg.libXfixes xorg.libXcomposite
        xorg.libXdamage xorg.libxcb xorg.xcbutil xorg.xcbutilwm xorg.xcbutilimage
        xorg.xcbutilkeysyms xorg.xcbutilrenderutil xorg.xcbutilcursor
        xorg.libSM xorg.libICE libxkbcommon wayland wayland-protocols
        mesa libGL vulkan-headers vulkan-loader libdrm xorg.libxshmfence
      ];

      # Windows GUI dependencies (MinGW cross-compiled)
      windowsGuiBuildInputs = [
        mingwPkgs.windows.pthreads
      ];

      # ─── Linux ───
      linuxRustLib = buildRustLib {};

      linux = buildGui {
        rustLib = linuxRustLib;
        qt6Static = linuxQt6;
        # Keep host ABI surface visible for runtime-loaded plugins.
        extraCmakeFlags = [ "-DCMAKE_EXE_LINKER_FLAGS=-Wl,--export-dynamic" ];
      };

      linuxTrace = buildGui {
        rustLib = linuxRustLib;
        qt6Static = linuxQt6;
        extraCmakeFlags = [ "-DSILENT_I18N_TRACE_IDS=ON" ];
      };

      # ─── Windows (MinGW cross-compilation) ───
      mingwPkgs = pkgs.pkgsCross.mingwW64;

      windowsRustLib = buildRustLib {
        rustTarget = "x86_64-pc-windows-gnu";
        extraNativeBuildInputs = [
          mingwPkgs.stdenv.cc
        ];
        extraBuildInputs = [
          mingwPkgs.windows.pthreads
        ];
      };

      windowsQt6Base = pkgs.callPackage "${qt_static}/nix/windows.nix" {
        qt6Source = qt_src;
        inherit qtVersion mingwPkgs;
        qt6HostTools = linuxQt6Base;
      };

      windowsQt6Svg = pkgs.callPackage "${qt_static}/nix/qtsvg-windows.nix" {
        qtSvgSource = qt_svg_src;
        qt6Linux = linuxQt6Base;
        qt6Windows = windowsQt6Base;
        inherit qtVersion mingwPkgs;
      };

      windowsQt6Raw = pkgs.callPackage "${qt_static}/nix/combine.nix" {
        qtbase = windowsQt6Base;
        qtsvg = windowsQt6Svg;
      };

      # Ensure all MinGW static archives have a symbol index.
      # Some precombined Qt archives can miss this index, which fails
      # cross-linking with: "archive has no index; run ranlib to add one".
      windowsQt6 = pkgs.runCommand "qt6-static-combined-${qtVersion}-windows-indexed"
        {
          nativeBuildInputs = [ mingwPkgs.stdenv.cc.bintools ];
        }
        ''
          cp -r ${windowsQt6Raw} $out
          chmod -R u+w $out
          find "$out" -type f -name '*.a' -print0 | xargs -0 -n1 x86_64-w64-mingw32-ranlib
        '';

      windows = buildGui {
        rustLib = windowsRustLib;
        qt6Static = windowsQt6;
        stdenvOverride = mingwPkgs.stdenv;
        guiBuildInputs = windowsGuiBuildInputs;
        # Windows host-symbol export for plugin DLLs is wired in CMakeLists.txt:
        # a curated export table (host ABI + qontrol + the full QtCore/Gui/Widgets/
        # Svg ABI) is generated from the libraries and produces
        # libsilent_host_exports.a. This stays under the PE 64K export-ordinal cap
        # that plain --export-all-symbols overflows (all Qt modules total >72K).
        extraCmakeFlags = [ ];
        # Replace Linux-specific link libraries with Windows equivalents
        postUnpackExtra = ''
          sed -i '/^\s*ssl$/d' CMakeLists.txt
          sed -i '/^\s*crypto$/d' CMakeLists.txt
          sed -i '/^\s*pthread$/d' CMakeLists.txt
          sed -i '/^\s*dl$/d' CMakeLists.txt
          sed -i '/^\s*udev$/d' CMakeLists.txt
          # Add Windows system libraries after the silent_rust line in target_link_libraries.
          # setupapi/cfgmgr32 back the rust serialport crate (SetupDi*/CM_Get*); they were
          # previously pulled in transitively by Qt, but whole-archiving Qt for the plugin
          # export drops that propagation, so link them explicitly.
          sed -i '/target_link_libraries/,/)/{ s/^\(\s*\)silent_rust$/\1silent_rust\n\1ws2_32\n\1bcrypt\n\1userenv\n\1ntdll\n\1crypt32\n\1setupapi\n\1cfgmgr32/; }' CMakeLists.txt
        '';
      };

      # ─── macOS cross-compilation helpers ───
      xcode = pkgs.darwin.xcode_12_2;
      llvmPkgs = pkgs.llvmPackages_18;
      sdkRoot = "${xcode}/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk";
      clangBuiltinInclude = "${llvmPkgs.clang-unwrapped.lib}/lib/clang/18/include";
      macosVersion = "11.0";

      # libc++ headers from LLVM 18 (Xcode 12.2's libc++ lacks C++20 <ranges>)
      libcxxInclude = "${llvmPkgs.libcxx.dev}/include/c++/v1";

      # Generate preBuildSetup script for macOS Rust cross-compilation
      macosRustSetup = targetArch:
        let
          rustTriple = "${targetArch}-apple-darwin";
          clangTarget = "${targetArch}-apple-macos${macosVersion}";
          envPrefix = builtins.replaceStrings ["-"] ["_"] (pkgs.lib.toUpper rustTriple);
          cargoEnvPrefix = builtins.replaceStrings ["-"] ["_"] rustTriple;
        in ''
          # Create cross-compilation wrappers for macOS ${targetArch}
          mkdir -p $TMPDIR/macos-cross

          # C compiler wrapper
          cat > $TMPDIR/macos-cross/cc-${rustTriple} << 'CCWRAPPER'
#!/bin/sh
args=""
skip_next=0
for arg in "$@"; do
    if [ $skip_next -eq 1 ]; then skip_next=0; continue; fi
    case "$arg" in
        --target=*|-target=*) continue ;;
        --target|-target) skip_next=1; continue ;;
        --gcc-toolchain=*|--sysroot=*) continue ;;
        --sysroot|-isysroot) skip_next=1; continue ;;
        *) args="$args $arg" ;;
    esac
done
exec ${llvmPkgs.clang-unwrapped}/bin/clang -target ${clangTarget} \
    -isystem ${clangBuiltinInclude} \
    -isystem ${sdkRoot}/usr/include \
    -isysroot ${sdkRoot} \
    -mmacosx-version-min=${macosVersion} \
    -fuse-ld=${llvmPkgs.lld}/bin/ld64.lld \
    -Wl,-platform_version,macos,${macosVersion},${macosVersion} \
    -Wl,-arch,${if targetArch == "aarch64" then "arm64" else "x86_64"} \
    $args
CCWRAPPER
          chmod +x $TMPDIR/macos-cross/cc-${rustTriple}

          # C++ compiler wrapper (with libc++ headers)
          # Uses LLVM 18 libc++ for C++20 support (<ranges>)
          cat > $TMPDIR/macos-cross/cxx-${rustTriple} << 'CXXWRAPPER'
#!/bin/sh
args=""
skip_next=0
for arg in "$@"; do
    if [ $skip_next -eq 1 ]; then skip_next=0; continue; fi
    case "$arg" in
        --target=*|-target=*) continue ;;
        --target|-target) skip_next=1; continue ;;
        --gcc-toolchain=*|--sysroot=*) continue ;;
        --sysroot|-isysroot) skip_next=1; continue ;;
        *) args="$args $arg" ;;
    esac
done
exec ${llvmPkgs.clang-unwrapped}/bin/clang++ -target ${clangTarget} \
    -nostdinc++ \
    -isystem ${libcxxInclude} \
    -isystem ${clangBuiltinInclude} \
    -isystem ${sdkRoot}/usr/include \
    -isysroot ${sdkRoot} \
    -stdlib=libc++ \
    -mmacosx-version-min=${macosVersion} \
    $args
CXXWRAPPER
          chmod +x $TMPDIR/macos-cross/cxx-${rustTriple}

          # Linker wrapper
          cat > $TMPDIR/macos-cross/ld-${rustTriple} << 'LDWRAPPER'
#!/bin/sh
args=""
has_arch=0
has_platform=0
has_syslibroot=0
for arg in "$@"; do
    case "$arg" in
        -arch) has_arch=1 ;;
        -platform_version) has_platform=1 ;;
        -syslibroot) has_syslibroot=1 ;;
        -dynamic-linker*|--dynamic-linker*) continue ;;
        --hash-style*|--eh-frame-hdr) continue ;;
        --as-needed|--no-as-needed|--build-id*) continue ;;
    esac
    args="$args $arg"
done
extra_args=""
if [ $has_arch -eq 0 ]; then extra_args="$extra_args -arch ${if targetArch == "aarch64" then "arm64" else "x86_64"}"; fi
if [ $has_platform -eq 0 ]; then extra_args="$extra_args -platform_version macos ${macosVersion} ${macosVersion}"; fi
if [ $has_syslibroot -eq 0 ]; then extra_args="$extra_args -syslibroot ${sdkRoot}"; fi
exec ${llvmPkgs.lld}/bin/ld64.lld $extra_args \
    -L ${sdkRoot}/usr/lib \
    $args
LDWRAPPER
          chmod +x $TMPDIR/macos-cross/ld-${rustTriple}

          ln -sf ${llvmPkgs.llvm}/bin/llvm-ar $TMPDIR/macos-cross/ar-${rustTriple}

          # Symlink ld wrapper so clang -B finds it
          ln -sf $TMPDIR/macos-cross/ld-${rustTriple} $TMPDIR/macos-cross/ld

          export CC_${cargoEnvPrefix}="$TMPDIR/macos-cross/cc-${rustTriple}"
          export CXX_${cargoEnvPrefix}="$TMPDIR/macos-cross/cxx-${rustTriple}"
          export AR_${cargoEnvPrefix}="${llvmPkgs.llvm}/bin/llvm-ar"
          export CARGO_TARGET_${envPrefix}_LINKER="$TMPDIR/macos-cross/cc-${rustTriple}"
          export SDKROOT="${sdkRoot}"

          # openssl-sys needs these for cross-compilation
          export AR="${llvmPkgs.llvm}/bin/llvm-ar"
          export RANLIB="${llvmPkgs.llvm}/bin/llvm-ranlib"
        '';

      # Generate CMake toolchain file + wrapper scripts for macOS GUI cross-compilation
      macosGuiPostUnpack = targetArch:
        let
          clangTarget = "${targetArch}-apple-macos${macosVersion}";
          archFlag = if targetArch == "aarch64" then "arm64" else "x86_64";
        in ''
          # Create macOS cross-compilation wrappers for CMake
          mkdir -p $TMPDIR/macos-gui-cross

          cat > $TMPDIR/macos-gui-cross/cc << 'GUICC'
#!/bin/sh
args=""
for arg in "$@"; do
    case "$arg" in
        --gcc-toolchain=*|--sysroot=*) continue ;;
        *) args="$args $arg" ;;
    esac
done
exec ${llvmPkgs.clang-unwrapped}/bin/clang -target ${clangTarget} \
    -isystem ${clangBuiltinInclude} \
    -isystem ${sdkRoot}/usr/include \
    -isysroot ${sdkRoot} \
    -mmacosx-version-min=${macosVersion} \
    -fuse-ld=${llvmPkgs.lld}/bin/ld64.lld \
    -Wl,-platform_version,macos,${macosVersion},${macosVersion} \
    -Wl,-arch,${archFlag} \
    $args
GUICC
          chmod +x $TMPDIR/macos-gui-cross/cc

          cat > $TMPDIR/macos-gui-cross/cxx << 'GUICXX'
#!/bin/sh
args=""
for arg in "$@"; do
    case "$arg" in
        --gcc-toolchain=*|--sysroot=*) continue ;;
        *) args="$args $arg" ;;
    esac
done
exec ${llvmPkgs.clang-unwrapped}/bin/clang++ -target ${clangTarget} \
    -nostdinc++ \
    -isystem ${libcxxInclude} \
    -isystem ${clangBuiltinInclude} \
    -isystem ${sdkRoot}/usr/include \
    -isysroot ${sdkRoot} \
    -stdlib=libc++ \
    -mmacosx-version-min=${macosVersion} \
    -fuse-ld=${llvmPkgs.lld}/bin/ld64.lld \
    -Wl,-platform_version,macos,${macosVersion},${macosVersion} \
    -Wl,-arch,${archFlag} \
    $args
GUICXX
          chmod +x $TMPDIR/macos-gui-cross/cxx

          cat > $TMPDIR/macos-gui-cross/toolchain.cmake << TOOLCHAIN
set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR ${if targetArch == "aarch64" then "arm64" else "x86_64"})
set(CMAKE_C_COMPILER $TMPDIR/macos-gui-cross/cc)
set(CMAKE_CXX_COMPILER $TMPDIR/macos-gui-cross/cxx)
set(CMAKE_AR ${llvmPkgs.llvm}/bin/llvm-ar)
set(CMAKE_RANLIB ${llvmPkgs.llvm}/bin/llvm-ranlib)
set(CMAKE_OSX_SYSROOT ${sdkRoot})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE NEVER)
# Cross-compilation: don't try to link test programs (they'd be macOS binaries)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
# macOS: clock_gettime/shm are in libSystem, no separate librt
set(HAVE_GETTIME ON CACHE BOOL "")
set(HAVE_SHM_OPEN_SHM_UNLINK ON CACHE BOOL "")
TOOLCHAIN
        '';

      # ─── macOS ARM (aarch64-apple-darwin) ───
      macosArmRustLib = buildRustLib {
        rustTarget = "aarch64-apple-darwin";
        extraNativeBuildInputs = [
          llvmPkgs.clang-unwrapped
          llvmPkgs.lld
          llvmPkgs.llvm
        ];
        preBuildSetup = macosRustSetup "aarch64";
      };

      macosArmQt6Base = pkgs.callPackage "${qt_static}/nix/macos.nix" {
        qt6Source = qt_src;
        inherit qtVersion xcode;
        qt6HostTools = linuxQt6Base;
        targetArch = "aarch64";
        llvmPackages = llvmPkgs;
      };

      macosArmQt6Svg = pkgs.callPackage "${qt_static}/nix/qtsvg-macos.nix" {
        qtSvgSource = qt_svg_src;
        qt6Linux = linuxQt6Base;
        qt6Macos = macosArmQt6Base;
        inherit qtVersion xcode;
        targetArch = "aarch64";
        llvmPackages = llvmPkgs;
      };

      macosArmQt6 = pkgs.callPackage "${qt_static}/nix/combine.nix" {
        qtbase = macosArmQt6Base;
        qtsvg = macosArmQt6Svg;
      };

      macosArm = buildGui {
        rustLib = macosArmRustLib;
        qt6Static = macosArmQt6;
        guiBuildInputs = [];
        preConfigureExtra = ''
          cmakeFlags="$cmakeFlags -DCMAKE_TOOLCHAIN_FILE=$TMPDIR/macos-gui-cross/toolchain.cmake -DCMAKE_FIND_ROOT_PATH=${macosArmQt6};${sdkRoot}"
        '';
        postUnpackExtra = ''
          # macOS plugin linking uses -bundle_loader + -undefined dynamic_lookup.
          # That applies when building plugin bundles, not the silent host binary.
          ${macosGuiPostUnpack "aarch64"}
          sed -i '/^\s*ssl$/d' CMakeLists.txt
          sed -i '/^\s*crypto$/d' CMakeLists.txt
          sed -i '/^\s*pthread$/d' CMakeLists.txt
          sed -i '/^\s*dl$/d' CMakeLists.txt
          sed -i '/^\s*udev$/d' CMakeLists.txt
          sed -i '/target_link_libraries/,/)/{ s/^\(\s*\)silent_rust$/\1silent_rust\n\1"-framework Security"\n\1"-framework SystemConfiguration"\n\1"-framework CoreFoundation"/; }' CMakeLists.txt
        '';
      };

      # ─── macOS x86_64 ───
      macosX86RustLib = buildRustLib {
        rustTarget = "x86_64-apple-darwin";
        extraNativeBuildInputs = [
          llvmPkgs.clang-unwrapped
          llvmPkgs.lld
          llvmPkgs.llvm
        ];
        preBuildSetup = macosRustSetup "x86_64";
      };

      macosX86Qt6Base = pkgs.callPackage "${qt_static}/nix/macos.nix" {
        qt6Source = qt_src;
        inherit qtVersion xcode;
        qt6HostTools = linuxQt6Base;
        targetArch = "x86_64";
        llvmPackages = llvmPkgs;
      };

      macosX86Qt6Svg = pkgs.callPackage "${qt_static}/nix/qtsvg-macos.nix" {
        qtSvgSource = qt_svg_src;
        qt6Linux = linuxQt6Base;
        qt6Macos = macosX86Qt6Base;
        inherit qtVersion xcode;
        targetArch = "x86_64";
        llvmPackages = llvmPkgs;
      };

      macosX86Qt6 = pkgs.callPackage "${qt_static}/nix/combine.nix" {
        qtbase = macosX86Qt6Base;
        qtsvg = macosX86Qt6Svg;
      };

      macosX86 = buildGui {
        rustLib = macosX86RustLib;
        qt6Static = macosX86Qt6;
        guiBuildInputs = [];
        preConfigureExtra = ''
          cmakeFlags="$cmakeFlags -DCMAKE_TOOLCHAIN_FILE=$TMPDIR/macos-gui-cross/toolchain.cmake -DCMAKE_FIND_ROOT_PATH=${macosX86Qt6};${sdkRoot}"
        '';
        postUnpackExtra = ''
          # macOS plugin linking uses -bundle_loader + -undefined dynamic_lookup.
          # That applies when building plugin bundles, not the silent host binary.
          ${macosGuiPostUnpack "x86_64"}
          sed -i '/^\s*ssl$/d' CMakeLists.txt
          sed -i '/^\s*crypto$/d' CMakeLists.txt
          sed -i '/^\s*pthread$/d' CMakeLists.txt
          sed -i '/^\s*dl$/d' CMakeLists.txt
          sed -i '/^\s*udev$/d' CMakeLists.txt
          sed -i '/target_link_libraries/,/)/{ s/^\(\s*\)silent_rust$/\1silent_rust\n\1"-framework Security"\n\1"-framework SystemConfiguration"\n\1"-framework CoreFoundation"/; }' CMakeLists.txt
        '';
      };

    in {
      packages.${system} = {
        inherit linux windows;
        linux-trace = linuxTrace;
        aarch64-apple-darwin = macosArm;
        x86_64-apple-darwin = macosX86;
        default = linux;
      };

      devShells.${system}.default = pkgs.mkShell {
        buildInputs = [
          # Rust (with cross-compilation targets)
          rustToolchain

          # C++ build
          pkgs.cmake
          pkgs.ninja
          pkgs.pkg-config

          # Libraries
          pkgs.openssl
          pkgs.openssl.dev
        ];

        shellHook = ''
          export CMAKE_PREFIX_PATH="${linuxQt6}"
          export QT_DIR="${linuxQt6}"
          echo "Silent dev shell"
          echo "  CMAKE_PREFIX_PATH=$CMAKE_PREFIX_PATH"
        '';
      };
    };
}
