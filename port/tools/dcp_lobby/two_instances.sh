#!/bin/bash
# Two-instance online test on one PC (Git Bash): mock lobby + host + joiner, headless, each instance on its own COPY of
# the user's save (the original is never touched). Drives the game's own online code with DCP_NET_TEST (hle/net_test.cpp)
# because the menu path needs online access in the save. Success = both logs show "MATCHING2 ... room", "signaling
# Established" and "RUDP socket 1 conn 1 connected" plus growing RUDP counters.
# usage: two_instances.sh <out_dir> <input_script_to_town> [frames=6000] [host_frame=1900] [join_frame=2000]
# DCP_LOBBY=ws://... or wss://... uses that lobby (e.g. the Worker under `wrangler dev`, server/lobby-worker) instead
# of starting the mock.
# DCP_TWO_MODE=friends: the joiner finds the host through friends instead of search (notes/map/friends.md): both test
# accounts are made friends first (dcp_lobby friend ...), the host publishes its presence blob (Toolkit2 SetPresence),
# the joiner hosts its own room, then runs the game's "Join Friends Room" path (DCP_NET_TEST=friend: GetPresence ->
# presence row -> JoinRoomA by the room id inside the blob). Success = joiner log "friend room(s)" >= 1 and
# "sceNpMatching2JoinRoomA" with the host's room id, then the usual signaling / RUDP lines.
set -u
OUT="$1"; INPUT="$2"; FRAMES="${3:-6000}"; HF="${4:-1900}"; JF="${5:-2000}"
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SAVES="${DCP_SAVES:-$USERPROFILE/Documents/Dragons Crown Pro/saves}"
mkdir -p "$OUT"
cd "$ROOT"
LOBBY="${DCP_LOBBY:-}"
LP=
if [ -z "$LOBBY" ]; then
    LOBBY=ws://127.0.0.1:47834/
    port/build/dcp_lobby.exe --port 47834 > "$OUT/lobby.log" 2>&1 &
    LP=$!
    sleep 0.5
fi
for who in host join; do
    rm -rf "$OUT/save_$who" "$OUT/user_$who"
    mkdir -p "$OUT/save_$who" "$OUT/user_$who"
    cp -r "$SAVES/." "$OUT/save_$who/"
done
JOIN_TEST=join
if [ "${DCP_TWO_MODE:-}" = friends ]; then
    JOIN_TEST=friend
    L=port/build/dcp_lobby.exe
    HU="$(cygpath -w "$OUT/user_host")"; JU="$(cygpath -w "$OUT/user_join")"
    HC=$($L friend "$LOBBY" "$HU" hostPlayer | sed -n 's/^code //p')
    $L friend "$LOBBY" "$JU" joinPlayer request "$HC" > "$OUT/friends.log"
    JA=$($L friend "$LOBBY" "$JU" joinPlayer | sed -n 's/^account //p')
    $L friend "$LOBBY" "$HU" hostPlayer accept "$JA" >> "$OUT/friends.log"
    cat "$OUT/friends.log"
fi
run() {  # who port frame
    T=$1; [ "$1" = join ] && T=$JOIN_TEST
    DCP_LOBBY="$LOBBY" DCP_NET_TEST=$T DCP_NET_TEST_FRAME=$3 DCP_NP_NAME=${1}Player DCP_NET_PORT=$2 \
    DCP_USER_DIR="$(cygpath -w "$OUT/user_$1")" DCP_SAVE_ROOT="$(cygpath -w "$OUT/save_$1")" DCP_NO_AUDIO=1 \
        timeout 900 port/build/dcp_run.exe "Game/Personal File Dump/eboot.bin" --input "$INPUT" --frames "$FRAMES" \
        > "$OUT/$1.log" 2>&1
}
run host 40001 "$HF" &
HP=$!
run join 40002 "$JF"
wait $HP
[ -n "$LP" ] && kill $LP 2> /dev/null
for who in host join; do
    echo "== $who"
    grep -E "LOBBY|NETTEST|MATCHING2 (created|joined|member|signal|owner|SetPresence|friend)|Toolkit2|JoinRoomA|RUDP|FAULT" \
        "$OUT/$who.log" | tail -24
done
