#!/usr/bin/env bash
# Run every branch's verification script against the merged tree.
# Usage: nix develop -c ./verify-all.sh
# Needs a working audio sink; the timebase and headroom checks play quiet tones.
set -u
cd "$(dirname "$0")"

fail=0
for s in verify-*.sh; do
    [ "$s" = "verify-all.sh" ] && continue
    name=${s#verify-}; name=${name%.sh}
    printf '=== %-26s ' "$name"
    if out=$(timeout 420 "./$s" 2>&1); then
        echo PASS
    else
        echo "FAIL (rc=$?)"
        echo "$out" | tail -20 | sed 's/^/      /'
        fail=1
    fi
done

echo
[ $fail -eq 0 ] && echo "ALL CHECKS PASSED" || echo "SOME CHECKS FAILED"
exit $fail
