#!/bin/sh
# check-coop-compat.sh -- did something co-op depends on change without a co-op version bump?
#
# Co-op pairs two copies by VSP_COOP_PROTOCOL and VSP_COOP_COMPAT (efcode/sys/sys_coop_net.h), not
# by the app version, so a patch release keeps playing with the releases before it
# (docs/coop-compat.md). This compares the tree with the last release tag (macos-v*). When files
# both copies must agree on changed and neither number moved, it fails, unless a commit since the
# tag says why the change is safe with a trailer line:
#
#     Coop-Compat: unchanged (only the host runs this; the messages and the replica are the same)
#
# Usage: tools/check-coop-compat.sh [base]   (base: a tag or commit; default the last macos-v* tag
#        before HEAD). Needs the tags and history (CI: actions/checkout with fetch-depth: 0).
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
NET=EFAndroid-SP/app/jni/efcode/sys/sys_coop_net.h

BASE="${1:-}"
if [ -z "$BASE" ]; then
	# HEAD^: on the release tag's own commit, compare with the release before it
	BASE="$(git describe --tags --abbrev=0 --match 'macos-v[0-9]*' HEAD^ 2>/dev/null || true)"
fi
if [ -z "$BASE" ]; then
	echo "check-coop-compat: no macos-v* tag before HEAD (fetch the tags); nothing to compare" >&2
	exit 0
fi

define() {	# define <rev> <name>: the value of #define <name> in sys_coop_net.h at <rev>
	git show "$1:$NET" 2>/dev/null | sed -n "s/^#define[[:space:]]*$2[[:space:]]*\([^[:space:]]*\).*/\1/p" | tr -d '"' | head -1
}
oldCompat="$(define "$BASE" VSP_COOP_COMPAT)"
# a release from before 1.5.1 has no VSP_COOP_COMPAT: it sent its app version, the tag's
[ -n "$oldCompat" ] || oldCompat="${BASE#macos-v}"
newCompat="$(define HEAD VSP_COOP_COMPAT)"
oldProto="$(define "$BASE" VSP_COOP_PROTOCOL)"
newProto="$(define HEAD VSP_COOP_PROTOCOL)"
if [ -z "$newCompat" ] || [ -z "$newProto" ]; then
	echo "check-coop-compat: no VSP_COOP_COMPAT / VSP_COOP_PROTOCOL in $NET at HEAD (commit first)" >&2
	exit 1
fi

if [ "$oldProto" != "$newProto" ] || [ "$oldCompat" != "$newCompat" ]; then
	echo "check-coop-compat: co-op version moved since $BASE (protocol $oldProto -> $newProto, compat $oldCompat -> $newCompat): copies before it won't pair, as intended"
	exit 0
fi

# What host and guest must agree on (docs/coop-compat.md, "When to bump"). Host-only game logic
# (most of efgame/src/coop, AI, scripts) and guest-only code (input, sound, rendering, the
# launchers' look) are not here: they change nothing the other copy sees.
SENSITIVE='^EFAndroid-SP/app/jni/efcode/sys/sys_coop_net\.[ch]$
^EFAndroid-SP/app/jni/efcode/sys/sys_coop_host\.c$
^EFAndroid-SP/app/jni/efcode/sys/sys_coop_compat\.h$
^EFAndroid-SP/app/jni/efcode/sys/sys_osx_coop\.[hm]$
^EFAndroid-SP/app/jni/efcode/launcher/lc_coop\.[ch]$
^efgame/src/game/coop_bridge\.h$
^efgame/src/coop/coop_replica\.cpp$
^efgame/src/coop/coop_predict\.cpp$
^efgame/src/coop/coop_lagcomp\.cpp$
^efgame/src/coop/coop_net\.cpp$
^efgame/src/game/bg_[^/]*$
^efgame/src/game/g_local\.h$
^efgame/src/game/g_shared\.h$
^efgame/src/game/b_public\.h$
^efgame/src/game/fields\.h$
^efgame/src/game/g_savegame\.cpp$
^efgame/src/game/q_shared\.h$
^efgame/src/cgame/cg_camera\.h$'

changed="$(git diff --name-only "$BASE" HEAD | grep -E "$(printf '%s' "$SENSITIVE" | paste -sd'|' -)" || true)"
if [ -z "$changed" ]; then
	echo "check-coop-compat: nothing co-op depends on changed since $BASE (co-op $newProto / $newCompat)"
	exit 0
fi

reasons="$(git log --format=%B "$BASE..HEAD" | grep -E '^Coop-Compat:[[:space:]]*unchanged' || true)"
if [ -n "$reasons" ]; then
	echo "check-coop-compat: co-op files changed since $BASE, judged compatible (co-op $newProto / $newCompat):"
	printf '%s\n' "$reasons" | sed 's/^/  /'
	exit 0
fi

echo "check-coop-compat: these files changed since $BASE, but VSP_COOP_COMPAT ($newCompat) and VSP_COOP_PROTOCOL ($newProto) did not:" >&2
printf '%s\n' "$changed" | sed 's/^/  /' >&2
cat >&2 <<EOF

Either the change breaks co-op with $BASE's copies: set VSP_COOP_COMPAT and LC_COOP_COMPAT to the
version being released (and bump both protocols if the messages changed). Or it doesn't: say why
in a commit message line "Coop-Compat: unchanged (<why>)". See docs/coop-compat.md.
EOF
exit 1
