# Extra CA certificates for the build

Drop PEM-encoded `*.crt` files in this directory if your network sits behind a
TLS-intercepting proxy or firewall. Everything here except this file and
`.gitignore` is ignored by git, so nothing environment-specific reaches the
(public) submodule remotes.

The build concatenates these certificates and passes them to `docker buildx`
as the `EXTRA_CA_CERTS` build argument. Build stages append them to
`/etc/ssl/certs/ca-certificates.crt`, which is what `apk`, `wget`, `curl` and
Go all read. When the directory is empty the build argument is empty and every
`RUN` that consumes it is a no-op, so builds on unintercepted networks are
unaffected.

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

Install the CA on a machine that already trusts it system-wide:

    cp /usr/local/share/ca-certificates/ambarella-enterprise-ca.crt tools/misc/ca/

## The BuildKit daemon needs it too

Build *stages* get the CA via `EXTRA_CA_CERTS`, but `ADD <git-url>` and
`ADD <https-url>` are resolved by the BuildKit daemon itself, outside any
stage. `make -f Makefile.eve buildkit-ca-image` builds a local
`moby/buildkit:<version>-localca` image with these certificates baked in, and
`ensure-builder` uses it automatically whenever this directory is non-empty.

If the builder was created before the certificates were added, recreate it:

    docker buildx rm eve-kernel-builder-v0.12.5
    make -f Makefile.eve ensure-builder
