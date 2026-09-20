# Presentation assets

This repository can turn ordinary developer-owned artwork and audio into the
launcher formats supported by this template. Conversion changes only the
repository's `sce_sys` files; it does not connect to or configure a console.

## What you can customize

| Experience | Source to provide | Generated console file |
| --- | --- | --- |
| Launcher tile | Square PNG, JPEG, or other FFmpeg-readable image | `sce_sys/icon0.png`, 512x512 PNG |
| Selection background | 16:9 image, preferably 3840x2160 | `sce_sys/pic0.dds`, 4K BC7 DX10 DDS |
| Launch/loading background | 16:9 image, preferably 3840x2160 | `sce_sys/pic1.dds`, 4K BC7 DX10 DDS |
| Selection music | MP3, M4A, AAC, WAV, FLAC, or a ready AT9 file | `sce_sys/snd0.at9`, 48 kHz stereo ATRAC9 |
| Displayed app name | `localizedParameters.<language>.titleName` in `sce_sys/param.json` | Shell-rendered text |

Retail-style custom-font logos and descriptions are catalog metadata, not
package assets. A synthetic homebrew concept has no retail catalog record, and
the supported `param.json` and image fields cannot create one. Catalog-database
modification is outside this portable template.

If desired, make a graphical logo part of the background. The Shell-rendered
`titleName` will still be present, so compose around it. Keep the main subject
on the right and leave breathing room on the left for the title and Play
button. Always verify the final composition on a TV because the Shell adds its
own crop, gradient, dimming, and controls.

## Install the image converter

The conversion is native and needs no Windows tool, no Wine, and no emulation.
FFmpeg scales the source to the exact console dimensions, and the pinned
[bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo) encoder writes the
DX10 BC7 container. Build the encoder once:

```bash
make assets-deps
```

That clones the pinned revision into the ignored `.deps/native/bc7enc_rdo`
cache and builds it with the host compiler; nothing is installed globally.
Intel's `ispc` is optional: `sudo pacman -S ispc` and rebuild, and the same
command uses bc7enc's higher-quality all-mode encoder. On a synthetic 1080p
test pattern it measures 49.3 dB RGB PSNR against 42.8 dB for the bundled
four-mode encoder.

The script then normalizes the four header words that differ between encoders,
so the file carries the same profile as the committed, hardware-validated
assets: one mipmap level and its flag, a depth of one, and straight alpha. The
148-byte DDS header is byte-for-byte the header of `sce_sys/pic0.dds`; only the
BC7 payload depends on the encoder. Use `--bc7enc` to point at a different
build of the encoder if you keep one outside `.deps/`.

## Prepare the icon and backgrounds

From the repository root:

```bash
./tools/prepare-assets.sh \
    --icon ~/art/my-icon.png \
    --background ~/art/my-background.png
```

The command normalizes the icon to 512x512 and the background to 3840x2160,
then produces the single-surface `DXGI_FORMAT_BC7_UNORM` (98), DX10 DDS
profile required by the template. `--background` is a shorthand that uses the
same image for both `pic0.dds` and `pic1.dds`.

Use separate artwork when the selected-app view and launch transition should
look different:

```bash
./tools/prepare-assets.sh \
    --selection-background ~/art/selected-app.png \
    --launch-background ~/art/launching.png
```

Hardware traces from the validated launcher path show `pic0.dds` in the
selected-app presentation and `pic1.dds` during the launching-game transition.
Shell and loader caching can delay visible changes, so refresh the title using
the loader's normal procedure after replacement.

The converter deliberately creates no mipmaps. Supply artwork with the correct
1:1 and 16:9 aspect ratios; resizing does not invent a good crop. Editable
sources are kept as `background-source.png` and
`launch-background-source.png`. The derived 4K PNG intermediates are discarded;
only `icon0.png`, `pic0.dds`, and `pic1.dds` are deployed.

## Prepare selection music

Selection music is optional. If `sce_sys/snd0.at9` is absent, the build simply
omits it.

If you already have a correctly encoded AT9 file, no encoder and no FFmpeg are
needed:

```bash
./tools/prepare-assets.sh --audio ~/music/selection.at9
```

For an MP3, M4A, AAC, WAV, or FLAC source the script needs:

- [FFmpeg](https://ffmpeg.org/) on `PATH` (`sudo pacman -S ffmpeg`).
- A compatible ATRAC9 encoder that you are legally permitted to use.

No ATRAC9 encoder is included, downloaded, or linked by this repository, and
none exists for Linux: FFmpeg can decode ATRAC9 but cannot encode it. This is
the one asset step with no native option, and the template does not need it —
the committed `sce_sys/snd0.at9` is already a valid, hardware-tested file, and
the build simply omits selection audio when the file is absent.

Re-encoding a new MP3, M4A, AAC, WAV, or FLAC source requires a compatible
ATRAC9 encoder that you are legally permitted to use. The commonly distributed
`ps4_at9tool` is a Windows executable, so that single step would need Wine;
treat it as an optional last resort rather than part of the workflow.

When you do have such an encoder, pass it with `--at9-tool`. FFmpeg prepares
the excerpt, the encoder turns it into ATRAC9, and the script adds the loop:

```bash
./tools/prepare-assets.sh \
    --audio ~/music/selection.mp3 \
    --at9-tool /path/to/your-at9-encoder
```

Choose a different excerpt with `--audio-start`:

```bash
./tools/prepare-assets.sh \
    --audio ~/music/selection.m4a \
    --audio-start 42.5 \
    --audio-duration 15 \
    --at9-tool /path/to/your-at9-encoder
```

The script strips metadata, normalizes toward -28 LUFS, and creates 48 kHz
stereo 16-bit PCM. The encoder it invokes produces ATRAC9 at 192 kb/s, and the
script adds a whole-track RIFF `smpl`
loop. It also writes `pubtools.loudnessSnd0` as `-28.00` when `param.json` is
present. The default 15-second, approximately 360 KB profile is intentionally
conservative, but `--audio-duration` accepts values through 87.3 seconds.

The 15-second value is a template policy, not a Shell duration limit. Offline
inspection of the Shell audio validator established these file-level limits:

- ATRAC9 in a RIFF/WAVE container at exactly 48 kHz.
- Mono at no more than 96 kb/s, or stereo at no more than 192 kb/s.
- A total RIFF file length no greater than 2,097,152 bytes (2 MiB).

The validator has no separate duration comparison. Duration follows from the
chosen channel count, bitrate, and ATRAC9 frame/container overhead. With the
template's 48 kHz stereo, 192 kb/s, whole-loop encoding, the largest complete
file below the ceiling is 2,096,808 bytes: 4,193,024 samples, or
87.354666667 seconds. One additional sample requires another 512-byte frame,
producing a 2,097,320-byte file that the Shell rejects. The converter caps
requested excerpts at 87.3 seconds to remain below that boundary. Short clips
remain quicker to prepare and smaller to distribute.

Only publish audio you own or have permission to distribute.

## Validate and build

Validate the current presentation files at any time:

```bash
./tools/prepare-assets.sh --validate-only
# or
make assets-check
```

The normal build runs the same validation automatically:

```bash
make
```

The validator rejects renamed PNG-as-DDS files, mipmapped or non-BC7 DDS
images, wrong dimensions, non-ATRAC9 audio, missing loop metadata, and audio
outside the supported sample-rate/channel/bitrate/size profile.
