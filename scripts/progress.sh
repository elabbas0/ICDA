#!/bin/sh
# A live line for the steps that take a while.
#
# The long steps of the build and of the release used to be silent: make, cmake
# and the push of a few hundred megabytes all ran with their output thrown away,
# so from the outside there was no way to tell a slow step from a hung one.  This
# puts one updating line per step on the terminal - the label, a clock, a bar -
# and keeps the full output in a log file for when something goes wrong.
#
#   . scripts/progress.sh
#
#   spin     "Building"      /tmp/m.log make            # sweeping bar
#   spin_pct "Compiling"     /tmp/c.log cmake --build . # bar from cmake's [ NN%]
#   progress "Copying" 37 214 "EFI/ICDA/KERNEL.BIN"     # inside a loop
#   note     "assembled 214 files"
#
# Nothing here changes what a step does or whether it fails: the step's exit
# status is the one that comes back out of spin, and a failure prints the tail
# of its log exactly as before.

PB_TTY=0
[ -t 1 ] && PB_TTY=1

pb_elapsed() {                                  # pb_elapsed <seconds> -> m:ss
    printf '%d:%02d' $(($1 / 60)) $(($1 % 60))
}

# A bar of <cells> characters: '#' filled, '>' the leading edge, '.' empty.
pb_bar() {
    _pb_pct=$1
    _pb_cells=$2
    [ "$_pb_cells" -ge 8 ] 2>/dev/null || _pb_cells=24
    [ "$_pb_pct" -ge 0 ] 2>/dev/null || _pb_pct=0
    [ "$_pb_pct" -le 100 ] 2>/dev/null || _pb_pct=100
    _pb_full=$((_pb_pct * _pb_cells / 100))
    _pb_part=$(((_pb_pct * _pb_cells % 100) * 8 / 100))
    _pb_out=''
    _pb_i=0
    while [ "$_pb_i" -lt "$_pb_cells" ]; do
        if [ "$_pb_i" -lt "$_pb_full" ]; then
            _pb_out="$_pb_out#"
        elif [ "$_pb_i" -eq "$_pb_full" ] && [ "$_pb_part" -gt 0 ]; then
            _pb_out="$_pb_out>"
        else
            _pb_out="$_pb_out."
        fi
        _pb_i=$((_pb_i + 1))
    done
    printf '[%s]' "$_pb_out"
}

# The percentage cmake prints on each progress line, or nothing.
pb_pct() {
    _pb_p=$(grep -o '\[[ 0-9]*%\]' "$1" 2>/dev/null | tail -1 | tr -cd '0-9' || true)
    [ -n "$_pb_p" ] && printf '%s' "$_pb_p" || printf '0'
}

pb_last() {
    _pb_l=$(grep -v '^[[:space:]]*$' "$1" 2>/dev/null | tail -1 | tr -d '\r' || true)
    # a step's last line is often a whole compiler command: keep it to one
    # readable fragment, the full text is in the log either way
    if [ ${#_pb_l} -gt 84 ]; then
        _pb_l="...${_pb_l}"
    fi
    printf '%s' "$_pb_l"
}

pb_erase() { [ "$PB_TTY" = 1 ] && printf '\r\033[K'; return 0; }

pb_reset() { PB_T0=$(date +%s); PB_TICK=0; }
PB_T0=$(date +%s)
PB_TICK=0

# One updating line.  <label> <start-seconds> <pct> <logfile>
pb_line() {
    _pb_t=$(pb_elapsed $(($(date +%s) - $2)))
    _pb_b=$(pb_bar "$3" 22)
    _pb_d=$(pb_last "$4")
    if [ "$PB_TTY" = 1 ]; then
        printf '\r\033[K  %-18s %6s  %s %3d%%  %s' "$1" "$_pb_t" "$_pb_b" "$3" "$_pb_d"
    else
        # not a terminal: one line every so often, so a piped log stays readable
        [ $((${PB_TICK:-0} % 5)) = 0 ] && printf '  %s %6s %3d%%  %s\n' "$1" "$_pb_t" "$3" "$_pb_d"
        PB_TICK=$((${PB_TICK:-0} + 1))
    fi
    return 0
}

# spin <label> <log> <cmd...>        bar sweeps, the step reports no percentage
# spin_pct <label> <log> <cmd...>    bar follows "[ NN%]" in the log
_pb_spin() {
    _pb_label=$1
    _pb_log=$2
    shift 2
    _pb_mode=${_PB_MODE:-sweep}
    _pb_rc="${_pb_log}.rc"
    _pb_start=$(date +%s)
    rm -f "$_pb_rc"
    : > "$_pb_log"
    # the subshell records its own status: testing the pid would race, because a
    # pid outlives an exited child until something waits for it
    ( "$@" >>"$_pb_log" 2>&1; printf '%s' $? > "$_pb_rc" ) &
    _pb_i=0
    while [ ! -f "$_pb_rc" ]; do
        if [ "$_pb_mode" = pct ]; then
            _pb_p=$(pb_pct "$_pb_log")
        else
            _pb_p=$(((_pb_i * 4) % 100))
        fi
        pb_line "$_pb_label" "$_pb_start" "$_pb_p" "$_pb_log"
        _pb_i=$((_pb_i + 1))
        if [ "$PB_TTY" = 1 ]; then sleep 0.2; else sleep 5; fi
    done
    pb_line "$_pb_label" "$_pb_start" 100 "$_pb_log"
    pb_erase
    _pb_out=$(cat "$_pb_rc" 2>/dev/null || printf 1)
    rm -f "$_pb_rc"
    if [ "$_pb_out" = 0 ]; then
        printf '  ok      %-18s %6s  %s\n' "$_pb_label" "$(pb_elapsed $(($(date +%s) - _pb_start)))" "$(pb_last "$_pb_log")"
        return 0
    fi
    printf '  FAILED  %-18s %6s  (exit %s)\n' "$_pb_label" "$(pb_elapsed $(($(date +%s) - _pb_start)))" "$_pb_out"
    printf -- '--- %s (last 20 lines) ---\n' "$_pb_log"
    tail -20 "$_pb_log"
    return "$_pb_out"
}

spin() {
    _PB_MODE=sweep
    _pb_spin "$@"
}

spin_pct() {
    _PB_MODE=pct
    _pb_spin "$@"
}

# progress <label> <done> <total> <detail>   - one line, no background job
progress() {
    _pg_p=$(( $2 * 100 / ($3 ? $3 : 1) ))
    _pg_t=$(pb_bar "$_pg_p" 22)
    if [ "$PB_TTY" = 1 ]; then
        printf '\r\033[K  %-18s %6s  %s %3d%%  %s' "$1" "$(pb_elapsed $(($(date +%s) - PB_T0)))" "$_pg_t" "$_pg_p" "$4"
    else
        printf '  %-18s %6s  %3d%%  %s\n' "$1" "$(pb_elapsed $(($(date +%s) - PB_T0)))" "$_pg_p" "$4"
    fi
    return 0
}

# how many entries a directory holds, for progress <label> n N
pb_count() {
    find "$1" -type f 2>/dev/null | wc -l | tr -d ' '
}

note() {
    pb_erase
    printf '  %s\n' "$*"
}