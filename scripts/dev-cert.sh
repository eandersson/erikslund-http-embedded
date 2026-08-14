#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

: "${OUTPUT_DIR:=dev-certificates}"
: "${DAYS:=365}"
: "${CURVE:=prime256v1}"
: "${FORCE:=0}"

IMAGE=erikslund-http-build

CERTIFICATE_NAME=server-certificate.pem
PRIVATE_KEY_NAME=server-key.pem

# Keep Git Bash from rewriting the container side of volume mounts.
export MSYS_NO_PATHCONV=1
SOURCE_DIR=$(pwd -W 2>/dev/null || pwd)

mkdir -p "$OUTPUT_DIR"

if [ -f "$OUTPUT_DIR/$CERTIFICATE_NAME" ] && [ -f "$OUTPUT_DIR/$PRIVATE_KEY_NAME" ] &&
        [ "$FORCE" != "1" ]; then
    echo "==> already present in $OUTPUT_DIR/ (FORCE=1 to replace)"
else
    echo "==> image"
    docker build -t "$IMAGE" docker

    echo "==> openssl: self-signed certificate for localhost, 127.0.0.1 and ::1, $DAYS days"
    docker run --rm \
        --user "$(id -u):$(id -g)" \
        -v "$SOURCE_DIR/$OUTPUT_DIR:/out" \
        "$@" \
        --entrypoint openssl \
        "$IMAGE" \
        req -x509 -noenc \
        -newkey ec -pkeyopt "ec_paramgen_curve:$CURVE" \
        -keyout "/out/$PRIVATE_KEY_NAME" \
        -out "/out/$CERTIFICATE_NAME" \
        -days "$DAYS" \
        -subj "/CN=localhost" \
        -addext "subjectAltName=DNS:localhost,IP:127.0.0.1,IP:0:0:0:0:0:0:0:1" \
        -addext "basicConstraints=critical,CA:FALSE" \
        -addext "keyUsage=critical,digitalSignature,keyEncipherment" \
        -addext "extendedKeyUsage=serverAuth"

    chmod 600 "$OUTPUT_DIR/$PRIVATE_KEY_NAME"
fi

echo
echo "certificate  $OUTPUT_DIR/$CERTIFICATE_NAME"
echo "private key  $OUTPUT_DIR/$PRIVATE_KEY_NAME"
echo
echo "The status_server example takes the pair as argv -- certificate chain first, then key:"
echo "    status_server $OUTPUT_DIR/$CERTIFICATE_NAME $OUTPUT_DIR/$PRIVATE_KEY_NAME"
echo
echo "Built by scripts/build.sh, the binary lives in the build volume rather than on the host, and"
echo "the example binds 127.0.0.1 on purpose -- so drive it from inside the same container:"
echo "    docker run --rm -it -v erikslund-http-build:/build \\"
echo "        -v \"$SOURCE_DIR/$OUTPUT_DIR:/certificates:ro\" \\"
echo "        --entrypoint /bin/bash $IMAGE"
echo "    # then, in the container:"
echo "    /build/cmake/examples/status_server \\"
echo "        /certificates/$CERTIFICATE_NAME /certificates/$PRIVATE_KEY_NAME &"
echo "    curl --cacert /certificates/$CERTIFICATE_NAME https://localhost:7778/healthz"
echo
echo "The E2E tier does not read this material: the stack in e2e/ mints a throwaway authority of"
echo "its own, so regenerating here cannot change what that suite tests."
echo
echo "For a configuration file rather than argv, the same pair goes in conf/http.yml as:"
echo "    listeners:"
echo "      - tls:"
echo "          enabled: true"
echo "          certificate_chain_file: \"$OUTPUT_DIR/$CERTIFICATE_NAME\""
echo "          private_key_file: \"$OUTPUT_DIR/$PRIVATE_KEY_NAME\""
