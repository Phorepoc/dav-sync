#!/bin/sh
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0
# Integration run on a Docker host: Radicale in a container (--cpus 1 --memory 512m), the ASan/UBSan test build in a gcc
# container on the same private network. Everything is removed at the end.
#   sh test/run.sh          plain HTTP run, then a TLS run (CA-verified and certificate-pinned) if .deps/mbedtls is built
set -u
cd "$(dirname "$0")/.."
NET=phe-davnet
cleanup() { docker rm -f phe-radicale phe-radicale-tls >/dev/null 2>&1; docker network rm $NET >/dev/null 2>&1; }
trap cleanup EXIT
cleanup
docker run --rm -v "$PWD":/w busybox:1.37 rm -rf /w/build   # root-owned leftovers of an earlier run
mkdir -p build
docker network create $NET >/dev/null || exit 1
docker build -q -t phe-radicale-img -f test/Dockerfile.radicale test >/dev/null || exit 1
docker run -d --name phe-radicale --network $NET --cpus 1 --memory 512m phe-radicale-img >/dev/null || exit 1
DRUN="docker run --rm --user $(id -u):$(id -g) --network $NET --cpus 1 --memory 512m -v $PWD:/w -w /w -e PHEDAV_USER=alice -e PHEDAV_PASS=secret"
WAIT='i=0; while [ $i -lt 40 ]; do (echo > /dev/tcp/$H/5232) 2>/dev/null && break; i=$((i+1)); sleep 0.5; done'
$DRUN --name phe-dav-test -e PHEDAV_URL=http://phe-radicale:5232/ gcc:14 bash -c "
    make clean >/dev/null && make integ-asan 2>&1 | grep -E 'warning|error'
    H=phe-radicale; $WAIT
    ./build/integ_asan"
rc=$?
if [ -f .deps/mbedtls/library/libmbedtls.a ]; then
    docker run -d --name phe-radicale-tls --network $NET --cpus 1 --memory 512m phe-radicale-img \
        python3 -m radicale --config /etc/radicale/config-tls >/dev/null || exit 1
    docker cp phe-radicale-tls:/etc/radicale/cert.pem build/ca.pem
    for mode in CA PIN; do
        echo "== TLS ($mode) =="
        $DRUN --name phe-dav-test-tls -e PHEDAV_URL=https://phe-radicale-tls:5232/ -e MODE=$mode gcc:14 bash -c "
            make integ-tls MBEDTLS=/w/.deps/mbedtls 2>&1 | grep -E 'warning|error'
            H=phe-radicale-tls; $WAIT
            if [ \$MODE = CA ]; then export PHEDAV_CA=/w/build/ca.pem; else export PHEDAV_PIN=\$(openssl x509 -in build/ca.pem -outform der | sha256sum | cut -d' ' -f1); fi
            ./build/integ_tls cal"
        [ $? -eq 0 ] || rc=1
    done
fi
exit $rc
