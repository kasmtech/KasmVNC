let pkgs = import (builtins.fetchTarball (builtins.fromJSON (builtins.readFile ./nixpkgs.json))) {};
in pkgs.buildEnv {
  name = "kasmvnc-archive-tools";
  paths = with pkgs; [ coreutils gnused gnutar xz ];
}
