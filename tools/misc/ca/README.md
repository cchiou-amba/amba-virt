# Extra CA certificates for the build

Drop PEM-encoded `*.crt` files in this directory if your network sits behind a
TLS-intercepting proxy or firewall. Everything here except this file,
`.gitignore`, `mkbuildkit.sh` and `find-ca-cert.sh` is ignored by git, so
nothing environment-specific reaches the (public) submodule remotes.

The build concatenates these certificates and passes them to `docker buildx`
as the `EXTRA_CA_CERTS` build argument. Build stages append them to
`/etc/ssl/certs/ca-certificates.crt`, which is what `apk`, `wget`, `curl` and
Go all read. When the directory is empty the build argument is empty and every
`RUN` that consumes it is a no-op, so builds on unintercepted networks -
including anyone outside the corporate firewall who clones this public
repository - are completely unaffected. There is nothing for that audience to
do; this whole mechanism only activates for people who deliberately add a
certificate.

## If you're outside the corporate firewall

Nothing to do. Leave this directory empty and the build behaves exactly as if
none of this existed.

## If you're a colleague behind the corporate firewall

Run the finder script, which searches this machine's own OS trust store
(populated by IT/MDM, domain join, etc.) for a matching certificate and copies
it in - it never fetches or trusts anything from the network, so there's no
"blindly trust whatever the network hands you" risk:

    ./tools/misc/ca/find-ca-cert.sh

If it reports no match, your machine doesn't have the certificate locally
trusted yet (a fresh VM, an unmanaged devbox, a container). Get the canonical
certificate from IT/security the same way you'd get any other credential -
don't extract it from a live intercepted connection, since that can't
distinguish your corporate proxy from a hostile one on some other network.
Once you have it:

    cp <certificate> tools/misc/ca/

## Ambarella corporate network

The firewall terminates TLS and re-signs with its own certificate
(`issuer=CN = 10.2.104.12`). Without the CA installed, the symptom is an
otherwise baffling build failure, e.g. `apk` reporting `no such package` for
packages that plainly exist:

    ERROR: https://dl-cdn.alpinelinux.org/alpine/v3.16/main: Permission denied
    WARNING: Ignoring ...: No such file or directory
    ERROR: unable to select packages:
      perl (no such package):

or BuildKit failing to fetch a git source:

    fatal: unable to access 'https://github.com/openzfs/zfs.git/':
      SSL certificate problem: self-signed certificate in certificate chain

## BuildKit daemons need it too

Build *stages* get the CA via `EXTRA_CA_CERTS`, but `ADD <git-url>` and
`ADD <https-url>` are resolved by a BuildKit daemon itself, outside any stage.
There are two such daemons in this build, and both need an image with the
certificates baked in - `mkbuildkit.sh` builds that image, and each is wired
to use it automatically whenever this directory is non-empty:

- The `eve-kernel` builder: `make -C eve-kernel -f Makefile.eve buildkit-ca-image`,
  used automatically by `ensure-builder`.
- linuxkit's builder (used while building `eve`): `make linuxkit-ca-image`
  at the repo root, which exports `LINUXKIT_BUILDER_IMAGE` for `linuxkit` to
  pick up.

If a builder was created before the certificates were added, recreate it so
it picks up the new image:

    docker buildx rm eve-kernel-builder-v0.12.5   # eve-kernel's builder
    docker rm -f linuxkit-builder                 # linuxkit's builder
