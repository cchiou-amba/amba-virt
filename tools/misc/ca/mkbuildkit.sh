#!/bin/sh
# Build a local BuildKit daemon image that trusts the CA certificates in this
# directory.
#
# Build stages receive the certificates through the EXTRA_CA_CERTS build
# argument, but that does not cover ADD <git-url> and ADD <https-url>, which
# BuildKit resolves in the daemon rather than inside a stage. The daemon
# therefore needs its own copy.
#
# Usage: mkbuildkit.sh <buildkit-version> <image-tag>
# Exits 0 without doing anything when no certificates are present or when the
# image already exists, so callers can invoke it unconditionally.
set -eu

version=${1:?usage: mkbuildkit.sh <buildkit-version> <image-tag>}
image=${2:?usage: mkbuildkit.sh <buildkit-version> <image-tag>}
dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

set -- "$dir"/*.crt
[ -e "$1" ] || exit 0
docker image inspect "$image" >/dev/null 2>&1 && exit 0

echo "Building $image with $# extra CA certificate(s)"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cp "$@" "$tmp/"
# Slimmer BuildKit images (v0.26.x) ship without update-ca-certificates, so
# fall back to appending the certificates to the bundle directly.
cat > "$tmp/Dockerfile" <<EOF
FROM moby/buildkit:$version
COPY *.crt /usr/local/share/ca-certificates/
RUN update-ca-certificates 2>/dev/null || \
    cat /usr/local/share/ca-certificates/*.crt >> /etc/ssl/certs/ca-certificates.crt
EOF
docker build -t "$image" "$tmp"
