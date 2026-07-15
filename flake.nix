{
  description = "audio-compare - compare audio files by ear";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      gst = with pkgs.gst_all_1; [
        gstreamer gst-plugins-base gst-plugins-good gst-plugins-bad gst-plugins-ugly
      ];
    in {
      packages.${system}.default = pkgs.stdenv.mkDerivation {
        pname = "audio-compare";
        version = "0.1.0";
        src = pkgs.lib.cleanSource ./.;
        nativeBuildInputs = with pkgs; [ meson ninja pkg-config wrapGAppsHook4 ];
        buildInputs = [ pkgs.gtk4 pkgs.libadwaita ] ++ gst;
      };

      devShells.${system}.default = pkgs.mkShell {
        packages = with pkgs; [ gcc pkg-config gtk4 libadwaita meson ninja ] ++ gst;
      };
    };
}
