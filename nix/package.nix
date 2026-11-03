{
  lib,
  config,
  stdenv,
  meson,
  ninja,
  pkg-config,
  wayland-scanner,
  wayland,
  wayland-protocols,
  libGL,
  libglvnd,
  freetype,
  fontconfig,
  cairo,
  pango,
  harfbuzz,
  libxkbcommon,
  sdbus-cpp_2,
  systemd,
  pipewire,
  pam,
  curl,
  libwebp,
  libjxl,
  libsndfile,
  glib,
  polkit,
  librsvg,
  libqalculate,
  libxml2,
  md4c,
  libsecret,
  libsodium,
  stb,
  fetchFromGitHub,
  nlohmann_json,
  tomlplusplus,
  libical,
  wireplumber,
  jemalloc,
  gst_all_1,
  libva,
  makeWrapper,
  git,
  gsettings-desktop-schemas,
  autoAddDriverRunpath,
  rev ? "unknown",
  # DEPRECATED: no longer affects the build; kept for `.override` compat.
  cudaSupport ? config.cudaSupport,
}:
let
  version = lib.fileContents ../VERSION;
  stb' = stb.overrideAttrs (_: {
    version = "unstable-2025-10-26";
    src = fetchFromGitHub {
      owner = "nothings";
      repo = "stb";
      rev = "f1c79c02822848a9bed4315b12c8c8f3761e1296";
      hash = "sha256-BlyXJtAI7WqXCTT3ylww8zoG0hBxaojJnQDvdQOXJPE=";
    };
  });
  # Runtime GStreamer plugins for ui.video: core elements, demuxers, VA-API hardware decoders
  # (zero-copy, in -bad), and the libav software decoders used as the fallback.
  gstPlugins = with gst_all_1; [
    gstreamer
    gst-plugins-base
    gst-plugins-good
    gst-plugins-bad
    gst-libav
  ];
in
lib.warnIf cudaSupport
  "noctalia: `cudaSupport` no longer has any effect (autoAddDriverRunpath is now always applied); this argument will be removed in the future."
  stdenv.mkDerivation {
  pname = "noctalia";
  inherit version;

  src = lib.cleanSource ./..;

  postPatch = ''
    substituteInPlace meson.build \
      --replace-fail "_git_revision_config.set('VCS_TAG', 'unknown')" "_git_revision_config.set('VCS_TAG', '${rev}')"
  '';

  postFixup = ''
    wrapProgram $out/bin/noctalia \
      --prefix PATH : ${lib.makeBinPath [ git ]} \
      --prefix XDG_DATA_DIRS : "${glib.getSchemaDataDirPath gsettings-desktop-schemas}" \
      --prefix GST_PLUGIN_SYSTEM_PATH_1_0 : "${lib.makeSearchPathOutput "lib" "lib/gstreamer-1.0" gstPlugins}"

    $out/bin/noctalia completions bash | install -D /dev/stdin $out/share/bash-completion/completions/noctalia
    $out/bin/noctalia completions zsh  | install -D /dev/stdin $out/share/zsh/site-functions/_noctalia
    $out/bin/noctalia completions fish | install -D /dev/stdin $out/share/fish/vendor_completions.d/noctalia.fish
  '';

  nativeBuildInputs = [
    meson
    ninja
    pkg-config
    wayland-scanner
    jemalloc
    makeWrapper
    autoAddDriverRunpath
  ];

  buildInputs = [
    wayland
    wayland-protocols
    libGL
    libglvnd
    freetype
    fontconfig
    cairo
    pango
    harfbuzz
    libxkbcommon
    sdbus-cpp_2
    systemd
    pipewire
    wireplumber
    pam
    curl
    libwebp
    libjxl
    libsndfile
    glib
    polkit
    librsvg
    libqalculate
    libxml2
    md4c
    libsecret
    libsodium
    stb'
    nlohmann_json
    tomlplusplus
    libical
    gst_all_1.gstreamer
    gst_all_1.gst-plugins-base
    # gstreamer-va-1.0 lets ui.video export VA decoder surfaces for zero-copy playback.
    gst_all_1.gst-plugins-bad
    libva
  ];

  mesonBuildType = "release";

  mesonFlags = [ "-Dtests=disabled" ];

  ninjaFlags = [ "-v" ];

  passthru = { inherit gstPlugins; };

  meta = with lib; {
    description = "A sleek, customizable desktop shell crafted for Wayland.";
    homepage = "https://github.com/noctalia-dev/noctalia";
    license = licenses.mit;
    platforms = platforms.linux;
    mainProgram = "noctalia";
  };
}
