#!/bin/bash
# hv_g7_ipsw_compare (host/g7_ipsw_check.c): the report of the IPSW SHA-256 comparison. Usage: tools/g7-ipsw-check-test.sh <build dir>
B=${1:?build dir}; mkdir -p "$B"
cd "$(dirname "$0")/.." || exit 1
${CC:-cc} -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Ihost host/g7_ipsw_check.c tests/test_g7_ipsw_check.c -o "$B/t" || exit 1
"$B/t" | tee "$B/out"; exit ${PIPESTATUS[0]}
