FROM nixos/nix:2.23.3

RUN mkdir /activen
WORKDIR /activen

COPY ./flake.nix /activen/flake.nix
COPY ./flake.lock /activen/flake.lock
RUN nix --extra-experimental-features nix-command --extra-experimental-features flakes develop -i

CMD ["nix", "--extra-experimental-features", "nix-command", "--extra-experimental-features", "flakes", "develop", "-i"]
