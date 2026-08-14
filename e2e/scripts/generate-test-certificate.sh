#!/usr/bin/env bash
set -euo pipefail

readonly TLS_DIRECTORY="${1:-/tls}"
readonly CERTIFICATE_FILE="${TLS_DIRECTORY}/server.crt"
readonly PRIVATE_KEY_FILE="${TLS_DIRECTORY}/server.key"

readonly SUBJECT_ALT_NAMES="DNS:status-server,DNS:status-allowlisted,DNS:status-restricted,\
DNS:localhost,IP:127.0.0.1"
readonly CERTIFICATE_SUBJECT="/CN=status-server"

readonly VALIDITY_DAYS=1

readonly KEY_CURVE=P-256

if [ -f "$CERTIFICATE_FILE" ] && [ -f "$PRIVATE_KEY_FILE" ]; then
    echo "ok: reusing the certificate already present in ${TLS_DIRECTORY}"
    exit 0
fi

openssl req -x509 -noenc \
    -newkey ec -pkeyopt "ec_paramgen_curve:${KEY_CURVE}" \
    -subj "$CERTIFICATE_SUBJECT" \
    -addext "subjectAltName=${SUBJECT_ALT_NAMES}" \
    -days "$VALIDITY_DAYS" \
    -keyout "$PRIVATE_KEY_FILE" \
    -out "$CERTIFICATE_FILE" 2>/dev/null

chmod 600 "$PRIVATE_KEY_FILE"
chmod 644 "$CERTIFICATE_FILE"

echo "ok: minted ${CERTIFICATE_FILE} for ${SUBJECT_ALT_NAMES}"
