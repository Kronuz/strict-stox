#!/bin/sh
# One verification chain for strict-stox's floating point path: the commands of tools/verify.cc, one log each, at nice 19.
# usage: [CHAIN_SET=full|long] tools/verify_chain.sh BINARY OUTDIR THREADS RAND_N MODES_N F32_STRIDE F32MID_STRIDE DMID_N [extra verify options...]
#   full: every command, every format. long: the digit-string and long-mantissa formats (14-16) and no f32 sweep, for a change that
#   touches only the code for more than 19 digits.
B=$1; O=$2; T=$3; RN=$4; MN=$5; FS=$6; FM=$7; DN=$8
shift 8
X="$*"
SET=${CHAIN_SET:-full}
mkdir -p "$O"
run() { # name args...
	n=$1; shift
	echo "=== $n: $B $* $X  ($(date))" | tee -a "$O/$n.log"
	nice -n 19 "$B" "$@" $X >> "$O/$n.log" 2>&1
	echo "exit $? ($(date))" | tee -a "$O/$n.log"
}
run hard hard -t "$T"
run syntax syntax -n 7 -t "$T"
run dmid dmid -n "$DN" -t "$T"
if [ "$SET" = full ]; then
	run rand rand -n "$RN" -t "$T"
	run modes modes -n "$MN" -t "$T"
	run f32 f32 --stride "$FS" --api-every 8 -t "$T"
else
	run rand rand -n "$RN" -t "$T" --fmt 14-16
	run modes modes -n "$MN" -t "$T" --fmt 14-16
fi
run f32mid f32mid --stride "$FM" --api-every 2 -t "$T"
echo "chain done $(date)" | tee -a "$O/chain.log"
