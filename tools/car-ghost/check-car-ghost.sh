#!/bin/bash
# check-car-ghost.sh [W-Rally.exe] [out dir]: is the ghost behind the car back?
#
# The ghost: the road a driving car uncovers took the car's own RT AO and shadow from the denoisers' history - pale
# copies of the exhaust and the bumper a frame of travel behind them (2026-11-07, shaders/rt_denoise.hlsl `standing`).
# This replays a drive on track 42 (the Impreza, chase camera; track42-impreza.phr, the user's), TAA off, and shoots
# every frame of three stretches in view 13 (RT AO) or view 11 (RT sun), each twice: as the game draws it, and with
# NGG_RT_NO_HISTORY=1 (this frame's rays alone). ghost.py takes the signed difference behind the car: a history may
# smooth the noise there, not shift it either way. Exits non-zero when a stretch is over its limit.
# Needs RBR at <RBR> (track-42.ngg, the Impreza), Python with numpy and Pillow. About 6 minutes on the dev iGPU.
here=$(cd "$(dirname "$0")" && pwd)
exe=${1:-$here/../../build/bin/W-Rally.exe}
exe=$(cd "$(dirname "$exe")" && pwd)/$(basename "$exe")
out=${2:-${TMPDIR:-/tmp}/car-ghost}
rbr=${RBR:-<RBR>}
lim=${GHOST_MAX:-1.0}       # the mean bias allowed, 0..255 grey (before the fix: 2-4; after: about 0)
mkdir -p "$out"
# Track 42's stage file: by its track id since the stages were renamed (fr_joux_verte), else the old name.
ngg42=$rbr/Ngg/fr_joux_verte.ngg; [ -f "$ngg42" ] || ngg42=$rbr/Ngg/track-42.ngg
fail=0
shoot() {   # <dir> <view> <from> <to> [ENV=val ...]
    local d=$out/$1 v=$2 from=$3 to=$4; shift 4
    rm -rf "$d"; mkdir -p "$d"
    (cd "$d" && env PHYS_DRIVE_STEP=0.0333333 PHYS_DRIVE_SHOTS=1 PHYS_DRIVE_OFF=vsync,taa NGG_DEBUG_VIEW=$v "$@" \
        timeout 600 "$exe" $rbr "$ngg42" --stage track-42_M --track fr_joux_verte@rbr \
        --sgc "$rbr\Cars\IMPREZA05\impreza05.sgc" --car-item impreza05@rbr --car cars/impreza05/physics/rbr/rbr/ \
        --physics-base physics/rbr/rbr/ --physics rbr --damage 2 --road dry --replay "$here/track42-impreza.phr" \
        --camera 0 --skip-to $from --exit-at $to --no-sound --no-codriver --out "$d/drive.phr" > "$d/log.txt" 2>&1)
}
# The stretches (view, first and last tick): 18 km/h on the walled straight (the user's shot, tick 7205) and the bends
# at 01:16 (tick 9604) in the AO; the sunny stretch at 00:17 at 38 km/h, the car's shadow on the road, in the sun's.
for s in "13 7000 7210" "13 9400 9606" "11 1100 1290"; do
    set -- $s
    n=v$1-$2
    shoot $n $1 $2 $3
    shoot $n-ref $1 $2 $3 NGG_RT_NO_HISTORY=1
    printf '%-10s ' $n
    python "$here/ghost.py" "$out/$n" "$out/$n-ref" --max $lim || fail=1
done
# And the golden replay (run 41, track-41, --step 0.1, the TV camera standing): copies of the car, a frame of travel
# apart, on the shaded road behind it (the sun's history - 2026-11-10). Where its files are (GOLDEN_DIR, the phys-data
# checkout); view 11, the road beside and behind the car at 116 km/h, blurred against the reference.
gd=${GOLDEN_DIR:-<phys-data>}
if [ -f "$gd/m5/track-41.ngg" ] && [ -f "$gd/m2/run-41-20261011-150633.phr" ]; then
    gold() {   # <dir> [ENV=val ...]
        local d=$out/$1; shift
        rm -rf "$d"; mkdir -p "$d"
        (cd "$d" && env PHYS_DRIVE_SHOTS=1 PHYS_DRIVE_OFF=vsync,taa NGG_DEBUG_VIEW=11 "$@" timeout 600 "$exe" $rbr             "$gd/m5/track-41.ngg" --stage track-41_M --step 0.1 --no-sound --overrides none             --replay "$gd/m2/run-41-20261011-150633.phr" --skip-to 4400 --exit-at 4830 --out "$d/drive.phr" > "$d/log.txt" 2>&1)
    }
    gold golden
    gold golden-ref NGG_RT_NO_HISTORY=1
    printf '%-10s ' golden
    python "$here/ghost.py" "$out/golden" "$out/golden-ref" --box 900,500,1920,1080 --blur 6 --max-abs ${GOLDEN_MAX:-4.0} || fail=1
else
    echo "golden    skipped: no $gd/m5/track-41.ngg or its run-41 replay"
fi
[ $fail = 0 ] && echo "no ghost behind the car" || echo "THE GHOST BEHIND THE CAR IS BACK (pictures in $out)"
exit $fail
