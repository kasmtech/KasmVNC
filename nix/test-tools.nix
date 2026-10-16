# Separate from the shipped server closure: tools for portable runtime/GPU checks.
let pkgs = import (builtins.fetchTarball (builtins.fromJSON (builtins.readFile ./nixpkgs.json))) {};
in pkgs.buildEnv {
  name = "kasmvnc-test-tools";
  postBuild = ''echo ${pkgs.mesa} > "$out/mesa-path";'';
  paths = with pkgs; [ bash coreutils curl python3 xorg.xdpyinfo xorg.xsetroot xorg.xmessage
    mesa-demos vulkan-tools (lib.setPrio 10 virtualgl) ffmpeg_6-headless openbox xterm dejavu_fonts ];
}
