#!/bin/bash
# Black-box test of deskControl: a headless mwc with simulated mouse and keyboard.
# What it switches is read from mwc's state file, what it shows from screenshots.
# It has its own sockets and touches no running session.
#
# Needs a built mwc with build/vinput, build/pngtool, build/workspacec, build/xclient
# and build/xdgc in MWC_DIR (by default ../mwc), grim and Xwayland. Output goes to
# build/ui.log, deskControl's own to build/ui-deskControl.log, the compositor's to
# build/ui-wayland.log.
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
for tool in build/vinput build/pngtool build/workspacec build/xclient build/xdgc; do
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
	local got= i
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

# Its frame: a bevel of 2, then a line of the face, active while it has the keyboard.
frame_is() { # description rrggbb
	local got= i
	for ((i = 0; i < 30; i++)); do
		WAYLAND_DISPLAY=wayland-0 grim "$SHOT" 2>/dev/null &&
			got=$(build/pngtool pixel "$SHOT" $((X0 + 2)) "$(cy 1)")
		[ "$got" = "$2" ] && { echo "ok: $1"; return 0; }
		sleep 0.1
	done
	cp "$SHOT" "$ERR_SHOT" 2>/dev/null
	fail "$1: the frame is $got, expected $2 (screenshot in $ERR_SHOT)"
}
# Whether it gets the keyboard on mapping is mwc's choice, and may come late: the
# frame has to agree with mwc's kbfocus as it is when the screenshot is taken.
frame_follows_focus() {
	local got= want= i
	for ((i = 0; i < 30; i++)); do
		refresh
		[ "$(get deskControl kbfocus)" = 1 ] && want=$SEL || want=$BG
		WAYLAND_DISPLAY=wayland-0 grim "$SHOT" 2>/dev/null &&
			got=$(build/pngtool pixel "$SHOT" $((X0 + 2)) "$(cy 1)")
		[ "$got" = "$want" ] &&
			{ echo "ok: mapped, the frame follows the keyboard focus"; return 0; }
		sleep 0.1
	done
	cp "$SHOT" "$ERR_SHOT" 2>/dev/null
	fail "the frame is $got, expected $want for kbfocus (screenshot in $ERR_SHOT)"
}
frame_follows_focus

face_is "desktop 1 is shown as the current one" 1 $SEL
face_is "the others are not" 3 $BG

# The label on the active colour, a mid-tone: black reads better on it than white.
# Counted inside the bevel, whose light half is near white.
ink_is() { # description button rrggbb-wanted rrggbb-not
	local yes= no= i
	for ((i = 0; i < 30; i++)); do
		if WAYLAND_DISPLAY=wayland-0 grim "$SHOT" 2>/dev/null; then
			yes=$(build/pngtool count "$SHOT" $(($(bx "$2") + 3)) $(($(by "$2") + 3)) \
				$((BW - 6)) $((BH - 6)) "$3")
			no=$(build/pngtool count "$SHOT" $(($(bx "$2") + 3)) $(($(by "$2") + 3)) \
				$((BW - 6)) $((BH - 6)) "$4")
			[ "$yes" -gt 0 ] && [ "$no" = 0 ] && { echo "ok: $1"; return 0; }
		fi
		sleep 0.1
	done
	cp "$SHOT" "$ERR_SHOT" 2>/dev/null
	fail "$1: button $2 has $yes pixels near $3 and $no near $4 (screenshot in $ERR_SHOT)"
}
ink_is "the current desktop's label is black" 1 000000 ffffff

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
# Dragged back onto it, the press counts again, as on a Motif push button.
do_input move "$(cx 3)" "$(cy 3)"
do_input down left
do_input move "$(cx 3)" $((Y0 + CH + 40))
do_input move "$(cx 3)" "$(cy 3)"
do_input up left
current_is "pressed, dragged off and back, the release switches" 3
do_input move "$(cx 2)" "$(cy 2)"
do_input click left
current_is "back to 2" 2
do_input move "$(cx 1)" "$(cy 1)"
do_input wheel 1
current_is "the wheel down goes to the next one" 3
do_input wheel -1
current_is "and up to the one before" 2

# ---- the keyboard ---------------------------------------------------------------------

check "deskControl has the focus" "[ \"\$(get deskControl kbfocus)\" = 1 ]"
frame_is "the frame is in the active colour" $SEL
# Another window takes the keyboard: the frame goes back; a click gives it again.
WAYLAND_DISPLAY=wayland-0 build/xdgc app=thief size=200x100 >"$SMOKE_DIR/thief.log" 2>&1 &
PID_thief=$!
check "another window takes the keyboard" \
	"[ \"\$(get thief kbfocus)\" = 1 ] && [ \"\$(get deskControl kbfocus)\" = 0 ]"
frame_is "the frame is inactive again" $BG
do_input move "$(cx 2)" "$(cy 2)"
do_input click left
check "a click gives deskControl the keyboard back" \
	"[ \"\$(get deskControl kbfocus)\" = 1 ]"
frame_is "and the frame is active again" $SEL
current_is "clicking the current desktop changes nothing" 2
kill "$PID_thief" 2>/dev/null
wait "$PID_thief" 2>/dev/null
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

# ---- a scaled output ---------------------------------------------------------------------

# At scale 2 it draws twice the pixels: the first one of the face beside the bevel is
# the face's colour, not a blend of the two from a buffer scaled up.
printf 'Mwm*deskControl*allDesktops: true\nMwm*deskControl*geometry: -0+0\n' \
	>"$XDG_CONFIG_HOME/mwc/mwmrc"
echo 'Mwm*HEADLESS-1*outputScale: 2' >>"$XDG_CONFIG_HOME/mwc/mwmrc"
reload_mwc
check "the output is scaled" "[ \"\$(global output_HEADLESS-1 scale)\" = 200 ]"
WAYLAND_DISPLAY=wayland-0 "$DC" >>"$DC_LOG" 2>&1 &
PID_dc=$!
check "deskControl is mapped again" "[ -n \"\$(get deskControl x)\" ]"
X0=$(get deskControl x) Y0=$(get deskControl y)
sharp=
for ((i = 0; i < 30; i++)); do
	WAYLAND_DISPLAY=wayland-0 grim "$SHOT" 2>/dev/null &&
		sharp=$(build/pngtool pixel "$SHOT" $((2 * ($(bx 1) + 2))) $((2 * $(cy 1))))
	[ "$sharp" = $SEL ] && break
	sleep 0.1
done
[ "$sharp" = $SEL ] || { cp "$SHOT" "$ERR_SHOT" 2>/dev/null
	fail "at scale 2 the face beside the bevel is $sharp, not $SEL (screenshot in $ERR_SHOT)"; }
echo "ok: sharp at scale 2"
kill "$PID_dc"
wait "$PID_dc" 2>/dev/null

# The cursor at scale 4. A theme has only some sizes, and its buffer scale must divide
# the image's size, or other compositors than mwc kill the client. Adwaita's nearest to
# 8 * 4 is 30, which 4 doesn't divide. mwc only logs it, so the test reads what
# deskControl asks for in its protocol log. The buffer then goes at scale 1, and a
# viewport shows it at 30 / 4, about 8, as large as mwc shows its own cursor.
WL_LOG=$DC_DIR/build/ui-wayland.log
# Last cursor shown: "set_cursor calls, buffer scale, width, height".
cursor_shown() {
	awk '
	match($0, /wl_shm_pool#[0-9]+\.create_buffer\(new id wl_buffer#[0-9]+, [0-9]+, [0-9]+, [0-9]+/) {
		split(substr($0, RSTART, RLENGTH), f, /[#(, ]+/)
		size[f[6]] = f[8] " " f[9]
	}
	match($0, /wl_pointer#[0-9]+\.set_cursor\([0-9]+, wl_surface#[0-9]+/) {
		split(substr($0, RSTART, RLENGTH), f, /[#(, ]+/)
		cur = f[5]; n++
	}
	match($0, /wl_surface#[0-9]+\.set_buffer_scale\([0-9]+/) {
		split(substr($0, RSTART, RLENGTH), f, /[#.()]+/)
		sc[f[2]] = f[4]
	}
	match($0, /wl_surface#[0-9]+\.attach\(wl_buffer#[0-9]+/) {
		split(substr($0, RSTART, RLENGTH), f, /[#.()]+/)
		if (f[2] == cur) last = n " " sc[cur] " " size[f[5]]
	}
	END { print last }' "$WL_LOG"
}
# The last size the cursor's viewport was given, "w h"; -1 -1 is none.
cursor_dest() {
	grep -o 'wp_viewport#[0-9]*\.set_destination(-\?[0-9]*, -\?[0-9]*)' "$WL_LOG" | tail -1 |
		sed 's/.*(\(.*\), \(.*\))/\1 \2/'
}
cursor_ok() { # set_cursor-calls-at-least scale
	local c s w h
	read -r c s w h <<<"$(cursor_shown)"
	[ -n "$h" ] && [ "$c" -ge "$1" ] && [ "$s" -ge 1 ] && [ $((w % s)) = 0 ] &&
		[ $((h % s)) = 0 ] && [ "$s" -le "$2" ]
}
printf 'Mwm*deskControl*allDesktops: true\nMwm*deskControl*geometry: +0+0\n' \
	>"$XDG_CONFIG_HOME/mwc/mwmrc"
echo 'Mwm*HEADLESS-1*outputScale: 4' >>"$XDG_CONFIG_HOME/mwc/mwmrc"
reload_mwc
check "the output is at scale 4" "[ \"\$(global output_HEADLESS-1 scale)\" = 400 ]"
XCURSOR_SIZE=8 WAYLAND_DEBUG=client WAYLAND_DISPLAY=wayland-0 "$DC" 2>"$WL_LOG" &
PID_dc=$!
check "deskControl is mapped at scale 4" "[ -n \"\$(get deskControl x)\" ]"
X0=$(get deskControl x) Y0=$(get deskControl y)
# vinput counts in the 1280x720 of the output's pixels.
do_input move $((4 * $(cx 1))) $((4 * $(cy 1)))
check "the cursor's buffer scale divides its size" "cursor_ok 1 4"
echo "   set_cursor calls, scale, size: $(cursor_shown)"
check "and the viewport shows 30 at 8, not 10" "[ \"\$(cursor_dest)\" = '8 8' ]"
# The pointer stays; the scale changes under it.
SHOWN=$(cursor_shown | cut -d' ' -f1)
sed -i 's/outputScale: 4/outputScale: 2/' "$XDG_CONFIG_HOME/mwc/mwmrc"
reload_mwc
check "the output is at scale 2" "[ \"\$(global output_HEADLESS-1 scale)\" = 200 ]"
check "the cursor is set again at scale 2 without the pointer moving" \
	"cursor_ok $((SHOWN + 1)) 2 && [ \"\$(cursor_shown | cut -d' ' -f2)\" = 2 ]"
check "there the scale divides it, and the viewport is unset" "[ \"\$(cursor_dest)\" = '-1 -1' ]"
kill "$PID_dc"
wait "$PID_dc" 2>/dev/null
[ "$("$DC" -v)" = "deskControl $(sed -n 's/^#define DESKCONTROL_VERSION "\(.*\)"/\1/p' "$DC_DIR/version.h")" ] ||
	fail "-v: $("$DC" -v)"
echo "ok: -v prints the version"
"$DC" -h 2>/dev/null || fail "-h: exit $?"
echo "ok: -h exits 0"
for a in "-v junk" "-x"; do
	"$DC" $a 2>/dev/null && fail "$a: exit 0"
	echo "ok: $a is refused"
done

kill -0 "$pid" || fail "mwc died"
stop_vinput
stop_mwc
echo "PASS: deskControl"
