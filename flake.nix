{
  description = "audio-compare - compare audio files by ear";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
      gstFor = pkgs: with pkgs.gst_all_1; [
        gstreamer gst-plugins-base gst-plugins-good gst-plugins-bad gst-plugins-ugly
      ];
    in {
      packages = forAllSystems (pkgs: {
        default = pkgs.stdenv.mkDerivation {
          pname = "audio-compare";
          version = "0.3.1";
          src = pkgs.lib.cleanSource ./.;
          nativeBuildInputs = with pkgs; [ meson ninja pkg-config wrapGAppsHook4 ];
          buildInputs = [ pkgs.gtk4 pkgs.libadwaita pkgs.libebur128 ] ++ gstFor pkgs;
        };
      });

      devShells = forAllSystems (pkgs: {
        # mkShell's stdenv supplies the C compiler (gcc on Linux, clang on macOS).
        default = pkgs.mkShell {
          packages = with pkgs; [ pkg-config gtk4 libadwaita libebur128 meson ninja ] ++ gstFor pkgs;
        };
      });
    };
}
