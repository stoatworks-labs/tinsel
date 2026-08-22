#!/usr/bin/env python3
"""
No control is silently dead.

A GLSL uniform whose name does not match the C++ is ignored without a word:
glGetUniformLocation returns -1 and glUniform on -1 is a documented no-op. So a
slider can be wired to nothing while the plugin compiles, links, loads and
renders perfectly. Nothing in a build catches it and nothing in the picture
looks wrong -- the control just does not do anything, which is indistinguishable
from not having noticed what it is for.

This renders each parameter at several positions and checks the picture
actually changed.

The awkward part is that many controls are conditional: Turns does nothing
unless Layout is Spiral, Dim does nothing unless Background is Dimmed Source,
Colour 2 does nothing unless the palette is the one that reads it. A naive sweep
reports those as dead and buries the one real failure in nine false ones, so
each such parameter carries the context it needs to mean anything. Those
contexts are the interesting content of this file -- if a parameter is added
without one and it turns out to be conditional, this will say so loudly.

    python3 tools/sweep.py [--binary build/tinseltest]
"""

import argparse
import hashlib
import pathlib
import subprocess
import sys
import tempfile

# What else has to be true for a parameter to have any effect at all. Without
# these the sweep reports a working control as dead.
CONTEXT = {
    "Turns": ["Layout=0"],                       # spiral only
    "Direction": ["Layout=2"],                   # the linear projection's angle
    "Dim": ["Background=2"],                     # dimmed source only
    "Colour 1": ["Palette=0"],                   # the two colour-driven palettes
    "Colour1_Green": ["Palette=0"],
    "Colour1_Blue": ["Palette=0"],
    "Colour 2": ["Palette=1"],
    "Colour2_Green": ["Palette=1"],
    "Colour2_Blue": ["Palette=1"],
    "Softness": ["Sensitivity=0.5"],             # a shoulder needs a threshold
    # Stability filters over time, so on a still picture it provably does
    # nothing: history and current hold the same number and every blend between
    # them returns it. Only noise makes the control mean anything, which is
    # exactly the condition it exists for.
    "Stability": ["Noise=0.10", "Frames=40", "Sensitivity=0.45"],
    "Source Tint": ["Background=1"],             # needs something to tint from
    # A moving pattern, and long enough for the movement to show. The harness
    # drives a synthetic 60fps clock, so 40 frames is two thirds of a second --
    # at the bottom of Speed's range that is still nothing, which is the point.
    "Speed": ["Pattern=7", "Frames=40"],
    "Intensity": ["Pattern=4"],                  # chase, whose duty it changes
    "Spread": ["Pattern=1", "Palette=2"],        # a gradient, on a palette
    "Reverse": ["Pattern=7", "Layout=2"],        # a comet along a linear strip
    # The sweep's positions only ever reach preset 1, which is deliberately
    # the plugin's own defaults ("Warm Twinkle") — applied over defaults it
    # provably changes nothing. Start away from the defaults so applying it
    # is visible.
    "Preset": ["Pattern=4"],
}

# Positions to try. Three rather than two: a control that is a no-op at both
# ends but not in the middle is rare, but Saturation and Mix are both centred
# and it costs one render to stop worrying about it.
POSITIONS = ["0.0", "0.5", "1.0"]

# Parameters with no scalar float to sweep. "Audio" is the FFT buffer: its
# float value is meaningless, so sweeping it proves nothing and reports a
# false dead. The harness feeds every render a synthetic spectrum instead, and
# "Audio Level" is the sweepable proof that the buffer reaches the lamps.
SKIP = {"Audio"}


def render(binary, out, settings, frames, noise):
    command = [binary, "--out", str(out), "--width", "320", "--height", "180",
               "--frames", str(frames), "--noise", str(noise)]
    for setting in settings:
        command += ["--set", setting]

    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"render failed: {result.stderr.strip()}")
    return hashlib.sha256(out.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="build/tinseltest")
    arguments = parser.parse_args()

    binary = pathlib.Path(arguments.binary)
    if not binary.exists():
        print(f"no {binary} -- build with -DTINSEL_BUILD_TOOLS=ON first")
        return 2

    listing = subprocess.run([str(binary), "--list"], capture_output=True, text=True)
    if listing.returncode != 0:
        print("could not list parameters:", listing.stderr.strip())
        return 2

    names = []
    for line in listing.stdout.splitlines()[1:]:
        parts = line.split(None, 1)
        if len(parts) == 2:
            names.append(parts[1].rsplit(None, 1)[0].strip())

    # The About block is a text field and browser buttons, declared last. They
    # never touch a pixel, so sweeping them only buries a real dead control.
    if "About" in names:
        names = names[: names.index("About")]

    if not names:
        print("no parameters found")
        return 2

    dead = []
    with tempfile.TemporaryDirectory() as directory:
        out = pathlib.Path(directory) / "sweep.png"

        for name in names:
            if name in SKIP:
                continue
            context = list(CONTEXT.get(name, []))

            # Frame count and source noise are forced through the same table,
            # because neither is a plugin parameter but both decide whether
            # certain controls can mean anything at all.
            frames = 12
            noise = 0.0
            for entry in list(context):
                if entry.startswith("Frames="):
                    frames = int(entry.split("=", 1)[1])
                    context.remove(entry)
                elif entry.startswith("Noise="):
                    noise = float(entry.split("=", 1)[1])
                    context.remove(entry)

            digests = set()
            for position in POSITIONS:
                try:
                    digests.add(render(binary, out, context + [f"{name}={position}"], frames, noise))
                except RuntimeError as error:
                    print(f"  {name}: {error}")
                    dead.append(name)
                    break
            else:
                mark = "ok" if len(digests) > 1 else "DEAD"
                if len(digests) == 1:
                    dead.append(name)
                print(f"  {mark:4}  {name}")

    print()
    if dead:
        print(f"{len(dead)} parameter(s) changed nothing: {', '.join(dead)}")
        print("either the uniform name does not match the shader, or the sweep")
        print("needs a CONTEXT entry saying what else has to be true.")
        return 1

    swept = len([n for n in names if n not in SKIP])
    print(f"all {swept} swept parameters measurably change the picture"
          f" ({len(names) - swept} buffer parameter(s) skipped)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
