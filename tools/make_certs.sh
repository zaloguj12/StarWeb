#!/usr/bin/env bash
# Generate a local StarWeb root CA and a localhost server certificate for star://.
# Output goes to certs/ (gitignored). Re-running reuses an existing root CA so the
# trust anchor stays stable, and regenerates the leaf. Pass --force to rebuild the
# root as well.
#
#.  certs/starweb_root.pem.  root CA cert.  -> trusted by the browser/client
#.  certs/starweb_root.key.  root CA key.   -> signs leaves; never share this
#.  certs/localhost.pem.     server cert.   -> served by stwp_server on 8490
#.  certs/localhost.key.     server key
set -euo pipefail

cd "$(dirname "$0")/.."
CERTS=certs
mkdir -p "$CERTS"

ROOT_KEY=$CERTS/starweb_root.key
ROOT_CRT=$CERTS/starweb_root.pem
LEAF_KEY=$CERTS/localhost.key
LEAF_CRT=$CERTS/localhost.pem
LEAF_CSR=$CERTS/localhost.csr

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

if [ "$FORCE" = 1 ] || [ ! -f "$ROOT_KEY" ] || [ ! -f "$ROOT_CRT" ]; then
    echo "Generating StarWeb root CA..."
    openssl ecparam -name prime256v1 -genkey -noout -out "$ROOT_KEY"
    openssl req -x509 -new -key "$ROOT_KEY" -sha256 -days 3650 \
        -subj "/O=StarWeb/CN=StarWeb Root CA" \
        -addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
        -addext "keyUsage=critical,keyCertSign,cRLSign" \
        -addext "subjectKeyIdentifier=hash" \
        -out "$ROOT_CRT"
else
    echo "Reusing existing root CA ($ROOT_CRT). Pass --force to regenerate it."
fi

echo "Generating localhost server certificate..."
openssl ecparam -name prime256v1 -genkey -noout -out "$LEAF_KEY"
openssl req -new -key "$LEAF_KEY" -sha256 \
    -subj "/O=StarWeb/CN=localhost" -out "$LEAF_CSR"

EXT=$(mktemp)
cat > "$EXT" <<'EOF'
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost,IP:127.0.0.1,IP:0:0:0:0:0:0:0:1
subjectKeyIdentifier=hash
authorityKeyIdentifier=keyid,issuer
EOF

openssl x509 -req -in "$LEAF_CSR" -CA "$ROOT_CRT" -CAkey "$ROOT_KEY" \
    -CAcreateserial -days 825 -sha256 -extfile "$EXT" -out "$LEAF_CRT"

rm -f "$EXT" "$LEAF_CSR"

echo
echo "Verifying chain:"
openssl verify -CAfile "$ROOT_CRT" "$LEAF_CRT"

echo
echo "Done. Serve with:  ./stwp_server --tls-port 8490 --cert $LEAF_CRT --key $LEAF_KEY"
