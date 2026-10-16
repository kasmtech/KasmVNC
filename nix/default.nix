{ pkgs ? import (builtins.fetchTarball (builtins.fromJSON (builtins.readFile ./nixpkgs.json))) {},
  src ? ../., webAssets ? ../builder/www, revision ? "local", version ? "1.5.1" }:
let
  source = pkgs.lib.cleanSourceWith {
    inherit src;
    filter = path: type:
      let rel = pkgs.lib.removePrefix (toString src + "/") (toString path);
      in pkgs.lib.cleanSourceFilter path type
        && !(builtins.elem rel [ "builder/build" "builder/www" "output" "kasmweb" "kasmvnc-functional-tests" "xorg.build" "nix" ".ci" ".gitlab-ci.yml" ])
        && !(pkgs.lib.hasPrefix "builder/" rel && !(rel == "builder/startup" || pkgs.lib.hasPrefix "builder/startup/" rel))
        && !(pkgs.lib.hasPrefix "result" rel);
  };
in import ./package.nix { inherit pkgs webAssets revision version; src = source; }
