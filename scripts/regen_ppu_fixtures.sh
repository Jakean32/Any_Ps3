#!/usr/bin/env bash
# Any_Ps3 — regenerate the PPC64 test fixtures with a real GNU assembler.
# Requires: powerpc64-linux-gnu-{as,ld,objdump,objcopy} on PATH (any binutils).
set -euo pipefail
cd "$(dirname "$0")/.."

AS=powerpc64-linux-gnu-as
LD=powerpc64-linux-gnu-ld
if ! command -v $AS > /dev/null; then
    cat <<'EOF'
No powerpc64 assembler found. Easiest install without root:
    apt-get download binutils-powerpc64-linux-gnu
    dpkg-deb -x binutils-powerpc64-linux-gnu_*_amd64.deb ~/ppctools
    export PATH=~/ppctools/usr/bin:$PATH
    export LD_LIBRARY_PATH=~/ppctools/usr/lib/x86_64-linux-gnu
EOF
    exit 1
fi

python3 tools/reference/gen_fixtures.py
echo "fixtures regenerated; rebuild with: make clean && make test"
