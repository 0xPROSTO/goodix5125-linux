#!/usr/bin/env bash
# Matcher regression check on the local capture sets (fingerprints: local only).
#
#   tools/evalset.sh [SETEVAL]     default build/seteval
#
# captures/real/{enroll,probe}: the owner's right-index fprintd enrolment and
# live probes; captures/ds/<finger>/: 5 fingers x 8 touches. Prints genuine
# scores and the impostor distribution. Accept threshold: GX_MATCH_THRESHOLD.
cd "$(dirname "$0")/.."
B=${1:-build/seteval}; DS=captures/ds; R=captures/real
mos() { grep mosaic | sed 's/.*mosaic://' | tr -s ' ' '\n' | grep .; }
gen=$($B $R/enroll $R/probe | mos | awk '$1>0')            # zeros = not-centred captures
gen2=$($B $R/enroll $DS/right-index | mos)
imp=$($B $R/enroll $DS/left-index $DS/left-thumb $DS/right-middle $DS/right-thumb | mos)
for e in $DS/*; do imp="$imp
$($B $e $(ls -d $DS/* | grep -vx $e) | mos)"; done
echo "genuine real : $(echo $gen | tr '\n' ' ')"
echo "genuine ds-ri: $(echo $gen2 | tr '\n' ' ')"
echo "impostor max : $(echo "$imp" | sort -n | tail -3 | tr '\n' ' ')  (n=$(echo "$imp" | wc -l))"
echo "impostor hist: $(echo "$imp" | sort -n | uniq -c | awk '{printf "%s:%s ", $2, $1}')"
