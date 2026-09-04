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
#   the demo   that demo/plugin.js still holds this repo's shader text, character
#              for character. The browser demo cannot include a C++ file, so the
#              GLSL lives there a second time; a change here that is not mirrored
#              there is invisible until the demo behaves unlike the plugin.
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

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler, before a host has to find out.
#
# A shader that will not compile presents to an operator as "the effect does
# nothing", with the real message buried in the diagnostics log -- so without
# this it is caught at run time, in a host, or not at all.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
	local dir bad=0 n=0 shader

	if ! command -v glslc >/dev/null 2>&1; then
		printf '   skipped: glslc not installed (brew install shaderc)\n'
		return 0
	fi

	dir="$( mktemp -d )"

	python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

# Where this repo keeps its GLSL.
FILES = [
	"source/Shaders.cpp",
]

# Shaders the plugin assembles at run time.
# Mirrors LightShaderSource() and EffectProbeShaderSource() in Shaders.cpp.
ASSEMBLED = {
	"LightShader":       [ "kLightPreamble", "kEffectLibrarySource", "kLightMain" ],
	"EffectProbeShader": [ "#version 410 core\n", "kEffectLibrarySource", "probeMain" ],
}

named, unnamed = {}, []
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(?:(\w+)\s*(?:\[\s*\])?\s*=\s*)?R"\((.*?)\)"', text, re.S ):
		if m.group( 1 ): named[ m.group( 1 ) ] = m.group( 2 )
		else:            unnamed.append( m.group( 2 ) )
	for m in re.finditer( r'(\w+)\s*=\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+);', text ):
		named.setdefault( m.group( 1 ), "".join(
			s.encode().decode( "unicode_escape" )
			for s in re.findall( r'"((?:[^"\\\n]|\\.)*)"', m.group( 2 ) ) ) )

def emit( name, body ):
	# The vertex shader is the one that writes gl_Position; everything else is a
	# fragment shader. glslc takes the stage from the extension.
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

def piece( p ):
	# An int indexes the raw strings that are not assigned to a name, in source
	# order. A literal starts with #version. Anything else names a constant
	# above -- and a name that has moved is a KeyError here, not a silent skip.
	if isinstance( p, int ):       return unnamed[ p ]
	if p.startswith( "#version" ): return p
	return named[ p ]

for name, body in named.items():
	if body.lstrip().startswith( "#version" ) and "void main" in body:
		emit( name, body )

for name, parts in ASSEMBLED.items():
	emit( name, "".join( piece( p ) for p in parts ) )
SHADERS_PY

	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations \
			   "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done

	if [ "$n" -eq 0 ]; then
		# No shaders at all is a FAILURE, not a pass. It means the extraction
		# above has lost track of where this repo keeps its GLSL, and a check
		# that silently looks at nothing is worse than no check.
		printf '   no shaders were extracted -- the extraction has gone stale\n'
		rm -rf "$dir"
		return 1
	fi

	if [ "$bad" -eq 0 ]; then
		printf '   %d shaders, all compile\n' "$n"
	fi
	rm -rf "$dir"
	return "$bad"
}

echo "== shaders: every one through a real GLSL compiler"
if ! shaders_compile; then
	failures+=("shaders")
fi
echo

# The parameter plumbing first: it needs no GPU, it takes a moment, and it is
# the half an external user actually got stuck on (vertigo issue #2).
echo "== presets: every factory preset survives every host behaviour"
if ./build/tinseltest --presets | tail -1; then
	:
else
	failures+=("presets")
fi

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
echo "== demo: the browser demo's GLSL against this repo's"
if python3 demo/tools/check_shaders.py | tail -1; then
	:
else
	echo "   *** demo/plugin.js is no longer running the plugin's shader"
	failures+=("demo shaders")
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

	# Captured, then matched from a herestring -- never `nm ... | grep -q`.
	# Under `set -o pipefail` a `grep -q` that finds its match exits
	# immediately, the writer upstream takes SIGPIPE, and the PIPELINE
	# reports failure even though the symbol is there. It is output-size
	# dependent, so it fires on the bigger binary first and looks
	# intermittent. A herestring is not a pipeline, so nothing can SIGPIPE.
	symbols=$( nm -gU "$bundle" 2>/dev/null || true )
	if grep -q '_plugMain' <<<"$symbols"; then
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
