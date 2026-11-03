{
  pkgs,
  noctalia,
}:
pkgs.mkShell {
  inputsFrom = [ noctalia ];

  nativeBuildInputs = with pkgs; [
    # Workflow & Hooks
    just
    lefthook

    # Build tools invoked directly by just recipes
    meson
    ninja
    pkg-config
    wayland-scanner

    # Formatting (required by justfile)
    llvmPackages_22.clang-tools
    llvmPackages_22.libclang
    gnugrep
    gnused
    findutils
    python3

    # Debugging
    gdb
  ];

  shellHook = ''
    # Point to local assets so binaries find resources without installation
    export NOCTALIA_ASSETS_DIR="$PWD/assets"
    # GStreamer plugins for ui.video, matching the packaged wrapper
    export GST_PLUGIN_SYSTEM_PATH_1_0="${pkgs.lib.makeSearchPathOutput "lib" "lib/gstreamer-1.0" noctalia.gstPlugins}"

    echo " Noctalia dev-shell | 'just --list' to see available tasks"
  '';
}
