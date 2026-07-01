#!/usr/bin/env bash
# Regenerate the SoftAP device-console TLS cert (IP 192.168.4.1 in SAN).
set -euo pipefail
cd "$(dirname "$0")"
openssl genpkey -algorithm RSA -out prvtkey.pem -pkeyopt rsa_keygen_bits:2048
openssl req -new -key prvtkey.pem -out server.csr -config server_cert.conf
openssl x509 -req -in server.csr -signkey prvtkey.pem -out servercert.pem \
  -days 3650 -extensions v3_req -extfile server_cert.conf
rm -f server.csr
echo "Wrote servercert.pem and prvtkey.pem (SAN: 192.168.4.1, espd.local)"
