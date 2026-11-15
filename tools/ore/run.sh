#!/bin/sh
# ORE read regression test.
#
#   SMELT_DIR=<oxyromon>/crates/smelt-rs sh tools/ore/run.sh
#
# Writes a synthetic three-track image with an SBI (tools/ore/make_fixture.py),
# makes an ORE of it with oxyromon's orers, then checks the ORE against the
# CUE it came from with tools/chd/chd_read_test, which compares any two
# images cdaccess_open_image opens: the TOC, and every raw sector of the
# program area with its subchannel, so the SBI's Q, the held pregap and the
# PREGAP the files do not hold are all compared. Built against libsmelt
# under AddressSanitizer + UBSan with leak detection.
#
# Needs cargo, python3 and an oxyromon checkout, whose smelt-rs SMELT_DIR
# names. Fails rather than skips when any is missing, as the CHD test does.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

[ -n "$SMELT_DIR" ] || { echo "SMELT_DIR not set" >&2; exit 1; }
command -v cargo >/dev/null || { echo "cargo not found" >&2; exit 1; }
command -v python3 >/dev/null || { echo "python3 not found" >&2; exit 1; }
SMELT_DIR=$(cd "$SMELT_DIR" && pwd)
OXYROMON=$(cd "$SMELT_DIR/../.." && pwd)

cargo build -q --release --manifest-path "$SMELT_DIR/Cargo.toml"
cargo build -q --release --manifest-path "$OXYROMON/Cargo.toml" -p ore-rs

CC=${CC:-cc}
CF=$(tools/harness_cflags.sh)
LC=libretro-common
CD=mednafen/cdrom
WORK=${TMPDIR:-/tmp}/ore_read_test.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

SRC="tools/chd/chd_read_test.c tools/chd/stub.c
     mednafen/general.c mednafen/error.c mednafen/cdstream.c
     $CD/CDAccess.c $CD/CDAccess_CCD.c $CD/CDAccess_Image.c
     $CD/CDAccess_PBP.c $CD/CDAccess_CHD.c $CD/CDAccess_ORE.c
     $CD/audioreader.c $CD/cdaccess_track.c $CD/CDUtility.c $CD/galois.c
     $CD/l-ec.c $CD/lec.c $CD/recover-raw.c $CD/edc_crc32.c
     $LC/formats/chd/rchd.c $LC/encodings/encoding_huffman.c
     $LC/encodings/encoding_rzstd.c $LC/encodings/encoding_deflate.c
     $LC/encodings/encoding_crc32.c $LC/encodings/encoding_utf.c
     $LC/formats/7z/r7z_lzma.c $LC/formats/flac/rflac.c
     $LC/formats/vorbis/rvorbis.c
     $LC/streams/file_stream.c $LC/streams/trans_stream.c
     $LC/streams/trans_stream_deflate.c $LC/streams/trans_stream_pipe.c
     $LC/streams/trans_stream_rzstd.c
     $LC/vfs/vfs_implementation.c $LC/vfs/vfs_implementation_cdrom.c
     $LC/cdrom/cdrom.c $LC/file/file_path.c $LC/file/file_path_io.c
     $LC/file/retro_dirent.c $LC/memmap/memalign.c $LC/memmap/memmap.c
     $LC/lists/string_list.c $LC/lists/dir_list.c
     $LC/formats/data_transfer.c $LC/hash/lrc_hash.c
     $LC/compat/compat_strl.c $LC/compat/fopen_utf8.c
     $LC/compat/compat_posix_string.c $LC/compat/compat_strcasestr.c
     $LC/string/stdstring.c $LC/string/rstrtod.c $LC/time/rtime.c
     $LC/rthreads/rthreads.c $LC/rthreads/retro_eventcount.c
     $LC/features/features_cpu.c $LC/queues/retro_spsc.c
     $LC/queues/retro_waitable_spsc.c"

$CC -O1 -g $CF -DHAVE_ORE -fsanitize=address,undefined \
    -fno-sanitize-recover=undefined \
    -I$LC/include -Imednafen -I. -I"$SMELT_DIR/include" \
    -o "$WORK/ore_read_test" $SRC "$SMELT_DIR/target/release/libsmelt.a" \
    -lm -lpthread -ldl -lrt -lutil

python3 tools/ore/make_fixture.py "$WORK/Game"
"$OXYROMON/target/release/orers" -o "$WORK" "$WORK/Game.cue" >/dev/null
# The ORE carries the SBI; the CUE's reference reads the one beside it
[ -f "$WORK/Game.ore" ] || { echo "orers wrote no Game.ore" >&2; exit 1; }
# and is recognised by its magic, whatever its extension
cp "$WORK/Game.ore" "$WORK/Game.cd1"

export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1
fail=0
for img in Game.ore Game.cd1; do
   "$WORK/ore_read_test" "$WORK/Game.cue" "$WORK/$img" | tail -1 \
      | grep -q PASS || { echo "FAIL: $img"; fail=1; }
done

if [ $fail = 0 ]; then
   echo "ore_read_test: all images PASS"
else
   "$WORK/ore_read_test" "$WORK/Game.cue" "$WORK/Game.ore" || true
   exit 1
fi
