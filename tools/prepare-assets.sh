#!/usr/bin/env bash
# ps5-native-app-boilerplate - Presentation asset preparation.
# Copyright (C) 2026 BlackBearReloaded
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Converts developer-owned artwork and audio into the launcher formats the
# template deploys, then validates the result. The work runs natively: FFmpeg
# scales the artwork, the pinned bc7enc_rdo encoder (tools/setup-asset-
# dependencies.sh) writes BC7, and Python normalizes the DDS header to the
# profile the Shell accepts. No Windows tool, Wine, or PowerShell is involved.
#
# Usage: tools/prepare-assets.sh [options]; see --help.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
output_directory="$root/sce_sys"
validate_only=0
icon=
background=
selection_background=
launch_background=
audio=
bc7enc=
ffmpeg=
at9_tool=
audio_start=0
audio_duration=15

usage() {
    cat <<'EOF'
usage: tools/prepare-assets.sh [options]

  --icon PATH                  Prepare sce_sys/icon0.png
  --background PATH            Use one image for pic0.dds and pic1.dds
  --selection-background PATH  Prepare pic0.dds from distinct artwork
  --launch-background PATH     Prepare pic1.dds from distinct artwork
  --audio PATH                 Copy a ready AT9 or prepare supported source audio
  --audio-start SECONDS        Start of the audio excerpt (default: 0)
  --audio-duration SECONDS     Excerpt duration (default: 15; maximum: 87.3)
  --bc7enc COMMAND             BC7 encoder name or path (default: the pinned build)
  --ffmpeg COMMAND             FFmpeg name or path (default: PATH lookup)
  --at9-tool COMMAND           Legally obtained compatible ATRAC9 encoder
  --output-directory PATH      Destination (default: sce_sys)
  --validate-only              Validate the current files without converting
  -h, --help                   Show this help

Artwork conversion is fully native: FFmpeg scales the source to the exact
console dimensions and the pinned BC7 encoder writes the DX10 DDS container.
Build the encoder once with tools/setup-asset-dependencies.sh.

Audio conversion still needs a compatible ATRAC9 encoder, which is not
available for Linux. The template ships selection audio already; supply a ready
.at9 file to replace it, or point --at9-tool at an encoder you may legally use.
EOF
}

need_value() {
    [[ $# -ge 2 && -n $2 ]] || {
        echo "missing value for $1" >&2
        exit 2
    }
}

while (($#)); do
    case "$1" in
        --icon) need_value "$@"; icon=$2; shift 2 ;;
        --background) need_value "$@"; background=$2; shift 2 ;;
        --selection-background) need_value "$@"; selection_background=$2; shift 2 ;;
        --launch-background) need_value "$@"; launch_background=$2; shift 2 ;;
        --audio) need_value "$@"; audio=$2; shift 2 ;;
        --bc7enc) need_value "$@"; bc7enc=$2; shift 2 ;;
        --ffmpeg) need_value "$@"; ffmpeg=$2; shift 2 ;;
        --at9-tool) need_value "$@"; at9_tool=$2; shift 2 ;;
        --audio-start) need_value "$@"; audio_start=$2; shift 2 ;;
        --audio-duration) need_value "$@"; audio_duration=$2; shift 2 ;;
        --output-directory) need_value "$@"; output_directory=$2; shift 2 ;;
        --validate-only) validate_only=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *)
            echo "unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

output_directory=$(realpath -m -- "$output_directory")

if ((validate_only)); then
    [[ -z $icon$background$selection_background$launch_background$audio$bc7enc$ffmpeg$at9_tool ]] || {
        echo "--validate-only cannot be combined with conversion options" >&2
        exit 2
    }
    exec bash "$root/tools/validate-assets.sh" "$output_directory"
fi

[[ -n $icon || -n $background || -n $selection_background || -n $launch_background || -n $audio ]] || {
    echo "supply an icon, background, or audio input, or use --validate-only" >&2
    exit 2
}
if [[ -n $background && (-n $selection_background || -n $launch_background) ]]; then
    echo "use --background by itself, or use --selection-background and --launch-background" >&2
    exit 2
fi

python3 - "$root" "$output_directory" "$icon" "$background" \
    "$selection_background" "$launch_background" "$audio" "$bc7enc" "$ffmpeg" \
    "$at9_tool" "$audio_start" "$audio_duration" <<'PY'
from pathlib import Path
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

(
    root_text, output_text, icon, background, selection_background,
    launch_background, audio, bc7enc, ffmpeg, at9_tool, audio_start,
    audio_duration,
) = sys.argv[1:13]

root = Path(root_text)
output = Path(output_text)
DDS_MIPMAP_COUNT = 0x20000
DDS_ALPHA_MODE_STRAIGHT = 1


def fail(message):
    raise SystemExit(f"prepare-assets: {message}")


def resolve_input(value, label):
    path = Path(value)
    if not path.is_file():
        fail(f"{label} file not found: {value}")
    return path.resolve()


def resolve_executable(value, names, help_text):
    if value:
        candidate = Path(value)
        if candidate.is_file():
            return str(candidate.resolve())
        found = shutil.which(value)
        if found:
            return found
        fail(f"executable not found: {value}")
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    fail(help_text)


def run(command, label):
    if subprocess.run(command).returncode != 0:
        fail(f"{label} failed")


def scale(tool, source, width, height, destination, label):
    run([tool, "-hide_banner", "-loglevel", "error", "-y", "-i", str(source),
         "-vf", f"scale={width}:{height}:flags=lanczos", "-frames:v", "1",
         "-pix_fmt", "rgba", str(destination)], f"{label} scaling")


def normalize_dds(path, width, height, label):
    """Force the header to the profile texconv produced and the Shell accepted.

    bc7enc_rdo writes a valid single-level DX10 BC7 file but leaves the mipmap
    count at zero and the alpha mode unknown, because it never generates mips
    and does not classify alpha. Both are header-only facts, so state them.
    """
    data = bytearray(path.read_bytes())
    if len(data) < 148 or bytes(data[:4]) != b"DDS ":
        fail(f"{label} is not a DDS file")
    if bytes(data[84:88]) != b"DX10":
        fail(f"{label} is not a DX10 DDS file")
    header_size = struct.unpack_from("<I", data, 4)[0]
    height_read, width_read = struct.unpack_from("<II", data, 12)
    mipmaps = struct.unpack_from("<I", data, 28)[0]
    dxgi_format, dimension, _misc, array_size, _misc2 = struct.unpack_from("<IIIII", data, 128)
    expected = 148 + (width // 4) * (height // 4) * 16
    if header_size != 124 or (width_read, height_read) != (width, height):
        fail(f"{label} must be a {width}x{height} DDS; found {width_read}x{height_read}")
    if mipmaps not in (0, 1):
        fail(f"{label} must not carry a mipmap chain; found {mipmaps} levels")
    if (dxgi_format, dimension, array_size) != (98, 3, 1):
        fail(f"{label} must be single-surface BC7_UNORM (98); found format "
             f"{dxgi_format}, dimension {dimension}, array size {array_size}")
    if len(data) != expected:
        fail(f"{label} is {len(data)} bytes; a {width}x{height} single-level BC7 "
             f"image requires exactly {expected}")
    flags = struct.unpack_from("<I", data, 8)[0]
    struct.pack_into("<I", data, 8, flags | DDS_MIPMAP_COUNT)
    struct.pack_into("<I", data, 24, 1)
    struct.pack_into("<I", data, 28, 1)
    struct.pack_into("<I", data, 144, DDS_ALPHA_MODE_STRAIGHT)
    path.write_bytes(data)


def convert_background(scale_tool, encoder, source, label, stem, work):
    preview = work / f"{stem}-source.png"
    scale(scale_tool, source, 3840, 2160, preview, label)
    image = work / f"{stem}.dds"
    run([encoder, "-q", "-g", str(preview), str(image)], f"{label} BC7 encoding")
    normalize_dds(image, 3840, 2160, label)
    shutil.copyfile(image, output / f"{stem}.dds")
    return preview


def number_text(value):
    return f"{value:.10g}"


def update_loudness(path):
    if not path.is_file():
        return
    with path.open(encoding="utf-8") as source:
        param = json.load(source)
    pubtools = param.get("pubtools")
    if not isinstance(pubtools, dict):
        pubtools = {}
        param["pubtools"] = pubtools
    if pubtools.get("loudnessSnd0") == "-28.00":
        return
    pubtools["loudnessSnd0"] = "-28.00"
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", dir=path.parent
    )
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as target:
            json.dump(param, target, indent=2, ensure_ascii=False)
            target.write("\n")
        os.replace(temporary_name, path)
    except BaseException:
        try:
            os.unlink(temporary_name)
        except FileNotFoundError:
            pass
        raise


if output == Path(output.anchor):
    fail("refusing to use a filesystem root as the output directory")
output.mkdir(parents=True, exist_ok=True)

try:
    start = float(audio_start)
    duration = float(audio_duration)
except ValueError:
    fail("--audio-start and --audio-duration must be numbers")
if not 0 <= start <= 86400:
    fail("--audio-start must be between 0 and 86400 seconds")
if not 0.1 <= duration <= 87.3:
    fail("--audio-duration must be between 0.1 and 87.3 seconds")

work = Path(tempfile.mkdtemp(prefix="ps5-native-assets-"))
try:
    wants_images = bool(icon or background or selection_background or launch_background)
    player = None
    if wants_images or (audio and Path(audio).suffix.lower() != ".at9"):
        player = resolve_executable(
            ffmpeg, ["ffmpeg"],
            "FFmpeg was not found; install it with: sudo pacman -S ffmpeg",
        )

    encoder = None
    if wants_images:
        default = root / ".deps" / "native" / "bc7enc_rdo" / "build" / "bc7enc"
        encoder = resolve_executable(
            bc7enc, [str(default), "bc7enc"],
            "the BC7 encoder is not built; run: make assets-deps",
        )

    if icon:
        scale(player, resolve_input(icon, "Icon"), 512, 512, output / "icon0.png", "icon")

    if background:
        preview = convert_background(
            player, encoder, resolve_input(background, "Background"),
            "background", "pic0", work,
        )
        shutil.copyfile(output / "pic0.dds", output / "pic1.dds")
        shutil.copyfile(preview, output / "background-source.png")
        shutil.copyfile(preview, output / "launch-background-source.png")
    else:
        if selection_background:
            preview = convert_background(
                player, encoder, resolve_input(selection_background, "Selection background"),
                "selection background", "pic0", work,
            )
            shutil.copyfile(preview, output / "background-source.png")
        if launch_background:
            preview = convert_background(
                player, encoder, resolve_input(launch_background, "Launch background"),
                "launch background", "pic1", work,
            )
            shutil.copyfile(preview, output / "launch-background-source.png")

    if audio:
        source = resolve_input(audio, "Audio")
        sound_output = output / "snd0.at9"
        if source.suffix.lower() == ".at9":
            if source != sound_output:
                shutil.copyfile(source, sound_output)
        else:
            encoded_encoder = resolve_executable(
                at9_tool, ["ps4_at9tool"],
                "a compatible ATRAC9 encoder was not found; supply a ready .at9 "
                "file, or an encoder you may legally use, with --at9-tool",
            )
            wav = work / "selection.wav"
            run([player, "-hide_banner", "-loglevel", "error", "-y",
                 "-ss", number_text(start), "-i", str(source),
                 "-t", number_text(duration), "-map_metadata", "-1",
                 "-af", "loudnorm=I=-28:LRA=11:TP=-2", "-ar", "48000", "-ac", "2",
                 "-c:a", "pcm_s16le", str(wav)], "audio preparation")
            encoded = work / "snd0.at9"
            run([encoded_encoder, "-e", "-br", "192", "-wholeloop", str(wav), str(encoded)],
                "ATRAC9 encoding")
            shutil.copyfile(encoded, sound_output)
        update_loudness(output / "param.json")
finally:
    shutil.rmtree(work, ignore_errors=True)
PY

bash "$root/tools/validate-assets.sh" "$output_directory"
