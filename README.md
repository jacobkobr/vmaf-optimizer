# vmaf-optimizer

Works out the right compression setting for a folder of game clips instead of
guessing at one.

OBS writes 1080p60 at 30-40 Mbps and most clips don't need anywhere near that.
The problem is the right CRF depends on the footage. A quiet menu screen and a
firefight compress completely differently, so one value across the whole library
either wastes space on the easy clips or smears the busy ones.

This measures instead of guessing. For each clip it cuts a 20 second sample, so
it isn't re-encoding the whole file four times over, encodes that sample at a
few CRF values, scores every one against the original with VMAF, and keeps the
smallest file that still clears a quality floor. If nothing clears it, it falls
back to the best score and says so in the report.

## Requires

ffmpeg and ffprobe on PATH, built with libvmaf:

    ffmpeg -version | findstr libvmaf

## Build

    g++ -std=c++17 -O2 -static vmaf_optimizer.cpp -o vmaf_optimizer.exe

## Usage

    vmaf_optimizer --folder .\clips

Compressed copies go to `clips\optimized\` and the originals are left alone. A
`vmaf_report.csv` lands next to them with the chosen CRF and score per clip.

| flag | |
| --- | --- |
| `--folder` | folder to scan, not recursive |
| `--min-vmaf` | quality floor |
| `--codec` | libx264 or libx265 |
| `--preset` | ffmpeg preset |
| `--crf` | values to try |
| `--replace` | shrink in place, originals moved to `_originals_backup` |
| `--debug` | log every ffmpeg command to ffmpeg_debug.log |

Run it with no arguments for the defaults.
