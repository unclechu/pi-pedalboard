{ pkgs ? import <nixpkgs> {} }:
pkgs.mkShell {
  nativeBuildInputs = [
    pkgs.gcc8 # as in Raspbian
    pkgs.pkg-config
    pkgs.gnumake
  ];
  buildInputs = [
    pkgs.jack2
    pkgs.glibc # for libm.so
  ];
}
