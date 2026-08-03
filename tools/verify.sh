#!/usr/bin/env bash
#
# Everything that can be checked without a host, in one go.
#
#   tools/verify.sh
#
# Four things get checked, and they fail in different ways:
#
#   --effects  the GLSL copy of the effect library against the C++ copy, at one
#              pixel per lamp, over every effect x seven times x five
#              intensities x three spreads. This is the one that matters: the
#              effects are the plugin, they exist twice, and nothing else
#              notices when the two drift apart. It has already caught one real
#              defect -- Theater Chase computing its lamp index with a float
#              mod, which on the GPU returns the spacing instead of zero at an
#              exact multiple and puts out the brightest lamp in the pattern.
#   --palettes that the palette table bakes at all. Not pass/fail; it is
#              written out so a verify run leaves a picture of what the
#              palettes were on the day.
#   --bench    the render cost. Also not pass/fail -- there is no threshold
#              worth asserting on somebody else's GPU -- but a verify run
#              leaves a timing on the record, which is what turns "it feels
#              slower" into a comparison.
#   sweep.py   that no control is silently dead. A GLSL uniform whose name does
#              not match the C++ is ignored without a word, so this is the only
#              thing standing between a typo and a shipped slider that does
#              nothing.
#   the binary that the macOS build is universal and still exports plugMain.
#              Checked with lipo and nm rather than by reading the build log,
#              because an arm64-only build logs as a success.
#
# The effect check reports a tolerance rather than demanding equality, and that
# is not a fudge: the GLSL specification allows three units in the last place
# for exp and gives sin no accuracy requirement at all, so the two
# implementations cannot agree bit for bit and a test that insisted would fail
# on every driver. See kEffectTolerance in tools/tinseltest/main.cpp for what
# the number is and what was measured to choose it.
set -uo pipefail

cd "$(dirname "$0")/.."

if [[ ! -x build/tinseltest ]]; then
	echo "build/tinseltest not found. Run:"
	echo "  cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build"
	exit 1
fi

failures=()

echo "== effects: GLSL against C++"
if ./build/tinseltest --effects | tail -2; then
	:
else
	failures+=("effects")
fi

echo
echo "== palettes: the table bakes"
if ./build/tinseltest --palettes /tmp/tinsel-palettes.png > /dev/null; then
	echo "   wrote /tmp/tinsel-palettes.png"
else
	failures+=("palettes")
fi

echo
echo "== sweep: no control silently dead"
if python3 tools/sweep.py > /tmp/tinsel-sweep.txt 2>&1; then
	tail -1 /tmp/tinsel-sweep.txt
else
	echo "   *** dead controls, see /tmp/tinsel-sweep.txt"
	tail -4 /tmp/tinsel-sweep.txt
	failures+=("sweep")
fi

echo
echo "== bench: the render cost, for the record"
./build/tinseltest --bench --frames 60 2>&1 | sed -n '3,8p'

echo
echo "== binary: universal, and exports plugMain"
bundle="build-universal/Tinsel.bundle/Contents/MacOS/Tinsel"
if [[ -f "$bundle" ]]; then
	architectures="$(lipo -archs "$bundle" 2>/dev/null)"
	echo "   architectures: $architectures"
	[[ "$architectures" == *arm64* && "$architectures" == *x86_64* ]] \
		|| failures+=("not universal: $architectures")

	if nm -gU "$bundle" 2>/dev/null | grep -q '_plugMain'; then
		echo "   exports _plugMain"
	else
		failures+=("no _plugMain export -- the host will load the bundle and find no plugins")
	fi
else
	echo "   skipped: no universal build. Run:"
	echo "     cmake -B build-universal -DCMAKE_BUILD_TYPE=Release && cmake --build build-universal"
fi

echo
if (( ${#failures[@]} == 0 )); then
	echo "all checks passed"
	exit 0
fi

echo "FAILURES:"
printf '  %s\n' "${failures[@]}"
exit 1
