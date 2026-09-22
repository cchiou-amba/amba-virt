#!/bin/sh
# Find a corporate CA certificate already trusted by this machine's OS and
# copy it into this directory, for colleagues behind a TLS-intercepting
# firewall who don't already have it in tools/misc/ca/.
#
# This only reads certificates the OS already trusts (installed by IT/MDM,
# domain join, etc.) - it never fetches or trusts anything from the network,
# so it carries none of the risk of capturing a certificate off the wire.
#
# Usage: find-ca-cert.sh [subject-substring]
# Defaults to matching "ambarella" (case-insensitive) in the certificate
# subject. Exits 1 with no output if nothing matches - ask IT/security for
# the canonical certificate in that case, the same way you would for any
# other credential.
set -eu

pattern=${1:-ambarella}
dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
found=0

matches() {
	# stdin: one PEM certificate. Prints its subject-derived filename on
	# stdout and returns 0 if the subject matches $pattern.
	cert=$(cat)
	subject=$(printf '%s' "$cert" | openssl x509 -noout -subject 2>/dev/null) || return 1
	case "$(printf '%s' "$subject" | tr '[:upper:]' '[:lower:]')" in
	*"$(printf '%s' "$pattern" | tr '[:upper:]' '[:lower:]')"*)
		printf '%s\n' "$cert" > "$tmp/match-$found.crt"
		found=$((found + 1))
		return 0
		;;
	esac
	return 1
}

split_and_scan() {
	# $1: a file that may hold one or more concatenated PEM certificates.
	[ -f "$1" ] || return 0
	awk '/-----BEGIN CERTIFICATE-----/{c=""} {c=c $0 "\n"} /-----END CERTIFICATE-----/{print c > "'"$tmp"'/cand.pem"; close("'"$tmp"'/cand.pem"); system("cat \"'"$tmp"'/cand.pem\""); print "===SPLIT==="}' "$1" \
		2>/dev/null | awk -v RS='===SPLIT===\n' 'NF{print > ("'"$tmp"'/piece" NR ".pem"); close("'"$tmp"'/piece" NR ".pem")}'
	for piece in "$tmp"/piece*.pem; do
		[ -f "$piece" ] || continue
		matches < "$piece" || true
		rm -f "$piece"
	done
}

# Linux: Debian/Ubuntu and RHEL/Fedora individual-file and bundle locations.
for f in /usr/local/share/ca-certificates/*.crt /usr/share/ca-certificates/*.crt \
         /etc/pki/ca-trust/source/anchors/*.crt /etc/pki/ca-trust/source/anchors/*.pem; do
	split_and_scan "$f"
done
split_and_scan /etc/ssl/certs/ca-certificates.crt
split_and_scan /etc/pki/tls/certs/ca-bundle.crt

# macOS: search the keychains IT-managed devices actually populate.
if command -v security >/dev/null 2>&1; then
	for kc in /Library/Keychains/System.keychain \
	          /System/Library/Keychains/SystemRootCertificates.keychain; do
		security find-certificate -a -p "$kc" 2>/dev/null > "$tmp/macos.pem" || continue
		split_and_scan "$tmp/macos.pem"
	done
fi

if [ "$found" -eq 0 ]; then
	echo "No locally-trusted certificate matching '$pattern' was found." >&2
	echo "Ask your IT/security team for the canonical certificate instead." >&2
	exit 1
fi

i=0
while [ "$i" -lt "$found" ]; do
	src="$tmp/match-$i.crt"
	cn=$(openssl x509 -in "$src" -noout -subject | sed -n 's/.*CN *= *//p' | tr ' /' '--')
	dest="$dir/${cn:-cert-$i}.crt"
	if [ -f "$dest" ] && cmp -s "$src" "$dest"; then
		echo "Already present: $dest"
	elif [ -f "$dest" ]; then
		echo "SKIPPED (differs from existing file, remove it first to replace): $dest" >&2
	else
		cp "$src" "$dest"
		echo "Wrote: $dest"
	fi
	i=$((i + 1))
done
