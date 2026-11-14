#!/bin/bash
# Black-box test of deskControl: a headless mwc with simulated mouse and keyboard.
# What it switches is read from mwc's state file, what it shows from screenshots.
# It has its own sockets and touches no running session.
#
# Needs a built mwc with build/vinput, build/pngtool, build/workspacec and
# build/xclient in MWC_DIR (by default ../mwc), grim and Xwayland. Output goes to
# build/ui.log, deskControl's own to build/ui-deskControl.log.
DC_DIR=$(cd "$(dirname "$0")/.." && pwd)
DC=${DC:-$DC_DIR/deskControl}
LOG=$DC_DIR/build/ui.log
DC_LOG=$DC_DIR/build/ui-deskControl.log
MWC_DIR=${MWC_DIR:-$DC_DIR/../mwc}
[ -x "$DC" ] || { echo "FAIL: $DC not built"; exit 1; }
[ -d "$MWC_DIR" ] || { echo "FAIL: no mwc in $MWC_DIR (set MWC_DIR)"; exit 1; }
mkdir -p "$DC_DIR/build"
cd "$MWC_DIR" || exit 1
. tests/lib.sh
require_tools grim Xwayland
for tool in build/vinput build/pngtool build/workspacec build/xclient; do
	[ -x "$tool" ] || { echo "FAIL: $MWC_DIR/$tool not built (make $tool)"; exit 1; }
done

export MWC_TEST_STATE=$SMOKE_DIR/state
export LC_ALL=C.UTF-8
unset MWC_INACTIVE MWC_ACTIVE DC_FONT
: >"$DC_LOG"
SHOT=$SMOKE_DIR/ui.png
ERR_SHOT=$DC_DIR/build/ui-last.png
BG=aeb2c3      # Motif's default background, the buttons' face
SEL=7387ad     # activeBackground: the current desktop
URGENT=e7a53b  # the background mixed 70 % towards amber

mkdir -p "$XDG_CONFIG_HOME/mwc"
printf 'Mwm*deskControl*allDesktops: true\nMwm*deskControl*geometry: -0+0\n' \
	>"$XDG_CONFIG_HOME/mwc/mwmrc"
start_mwc 'echo "$DISPLAY" > "$SMOKE_DIR/display"; exec sleep 300'
wait_for "the startup command ran" test -s "$SMOKE_DIR/display"
X_DISPLAY=$(cat "$SMOKE_DIR/display")
start_vinput
refresh

WAYLAND_DISPLAY=wayland-0 "$DC" >>"$DC_LOG" 2>&1 &
PID_dc=$!
check "deskControl is mapped" "[ -n \"\$(get deskControl x)\" ]"
sleep 0.3
refresh

# Only its own thin frame: mwc adds no border and no title bar.
[ "$(get deskControl bw)" = 0 ] && [ "$(get deskControl th)" = 0 ] ||
	fail "mwc decorates it: border $(get deskControl bw), title $(get deskControl th)"
echo "ok: no server-side decoration"

# Where its buttons are: 1 2 over 3 4, inside its frame of 3, PAD 6 around them,
# GAP 4 between, all the same size.
X0=$(get deskControl x) Y0=$(get deskControl y)
CW=$(get deskControl w) CH=$(get deskControl h)
IN=$((3 + 6))
BW=$(((CW - 2 * IN - 4) / 2)) BH=$(((CH - 2 * IN - 4) / 2))
[ "$BW" -gt 10 ] && [ "$BH" -gt 10 ] || fail "four buttons don't fit in ${CW}x$CH"
[ "$BW" -gt "$BH" ] || fail "${CW}x$CH is not two buttons wide and two high"
echo "ok: four buttons in two rows, ${BW}x$BH"
bx() { echo $((X0 + IN + ($1 - 1) % 2 * (BW + 4))); }
by() { echo $((Y0 + IN + ($1 - 1) / 2 * (BH + 4))); }
cx() { echo $(($(bx "$1") + BW / 2)); }
cy() { echo $(($(by "$1") + BH / 2)); }
# A point on a button's face, beside its label.
face_x() { echo $(($(bx "$1") + 4)); }

face_is() { # description button rrggbb
	local got i
	for ((i = 0; i < 30; i++)); do
		if WAYLAND_DISPLAY=wayland-0 grim "$SHOT" 2>/dev/null; then
			got=$(build/pngtool pixel "$SHOT" "$(face_x "$2")" "$(cy "$2")")
			[ "$got" = "$3" ] && { echo "ok: $1"; return 0; }
		fi
		sleep 0.1
	done
	cp "$SHOT" "$ERR_SHOT" 2>/dev/null
	fail "$1: button $2 is $got, expected $3 (screenshot in $ERR_SHOT)"
}
current_is() { # description n
	check "$1" "[ \"\$(global desktop current)\" = $2 ] && [ \"\$(get deskControl visible)\" = 1 ]"
}

face_is "desktop 1 is shown as the current one" 1 $SEL
face_is "the others are not" 3 $BG

# ---- the mouse ----------------------------------------------------------------------

do_input move "$(cx 2)" "$(cy 2)"
do_input click left
current_is "a click on 2 switches there, and deskControl stays in sight" 2
face_is "and shows it" 2 $SEL
face_is "1 is out again" 1 $BG
# A press that leaves the button before the release does nothing.
do_input move "$(cx 3)" "$(cy 3)"
do_input down left
do_input move "$(cx 3)" $((Y0 + CH + 40))
do_input up left
sleep 0.3
current_is "pressed and dragged off, nothing changes" 2
do_input move "$(cx 1)" "$(cy 1)"
do_input wheel 1
current_is "the wheel down goes to the next one" 3
do_input wheel -1
current_is "and up to the one before" 2

# ---- the keyboard ---------------------------------------------------------------------

check "deskControl has the focus" "[ \"\$(get deskControl kbfocus)\" = 1 ]"
# Its frame: a bevel of 2, then a line of the face, active while it has the keyboard.
for ((i = 0; i < 30; i++)); do
	WAYLAND_DISPLAY=wayland-0 grim "$SHOT" 2>/dev/null &&
		got=$(build/pngtool pixel "$SHOT" $((X0 + 2)) "$(cy 1)")
	[ "$got" = $SEL ] && break
	sleep 0.1
done
[ "$got" = $SEL ] || { cp "$SHOT" "$ERR_SHOT"; fail "its frame is $got, expected $SEL"; }
echo "ok: the frame is in the active colour"
do_input key 4
current_is "4 goes to desktop 4" 4
do_input key Right
sleep 0.2
current_is "Right at the last one stays there" 4
do_input key Left
current_is "Left goes to the one before" 3
do_input key 1
current_is "1 to the first" 1
do_input key alt+3
sleep 0.2
current_is "Alt+3 is not 3" 1

# ---- following what others do -----------------------------------------------------------

WAYLAND_DISPLAY=wayland-0 build/workspacec activate 3 >/dev/null || fail "switching from outside"
current_is "another program switches to 3" 3
face_is "deskControl follows" 3 $SEL
face_is "and 1 is out" 1 $BG

# A dialog opening for a window on desktop 1, while 3 is current: 1 wants attention.
DISPLAY=$X_DISPLAY build/xclient class=late size=200x150 dialog=latekid dialog_ms=2500 \
	>"$SMOKE_DIR/late.log" 2>&1 &
PID_late=$!
do_input key 1
current_is "back to 1 for the window to open there" 1
check "the window is there" "[ \"\$(get late desktop)\" = 1 ]"
do_input move "$(cx 3)" "$(cy 3)"
do_input click left
current_is "and away to 3 again" 3
check "its dialog came, on desktop 1" "[ \"\$(get latekid desktop)\" = 1 ] && [ \"\$(get latekid visible)\" = 0 ]"
face_is "desktop 1 is marked" 1 $URGENT
face_is "3 stays the current one" 3 $SEL
do_input move "$(cx 1)" "$(cy 1)"
do_input click left
current_is "going there" 1
face_is "takes the mark away" 1 $SEL
kill "$PID_late" 2>/dev/null
wait "$PID_late" 2>/dev/null

# ---- quitting --------------------------------------------------------------------------

do_input move "$(cx 1)" "$(cy 1)"
do_input click left
check "deskControl has the focus again" "[ \"\$(get deskControl kbfocus)\" = 1 ]"
do_input key q
for ((i = 0; i < 50; i++)); do
	kill -0 "$PID_dc" 2>/dev/null || break
	sleep 0.1
done
kill -0 "$PID_dc" 2>/dev/null && fail "q doesn't quit"
wait "$PID_dc"
[ $? = 0 ] || fail "deskControl didn't exit with 0"
echo "ok: q quits"
[ "$("$DC" -v)" = "deskControl $(sed -n 's/^#define DESKCONTROL_VERSION "\(.*\)"/\1/p' "$DC_DIR/version.h")" ] ||
	fail "-v: $("$DC" -v)"
echo "ok: -v prints the version"

kill -0 "$pid" || fail "mwc died"
stop_vinput
stop_mwc
echo "PASS: deskControl"
