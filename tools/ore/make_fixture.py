#!/usr/bin/env python3
"""Write a synthetic three-track CD image (CUE, a BIN a track, SBI) for the
ORE test, with what a Redump PlayStation dump has besides its tracks:

  Track 1  MODE2/2352  data sectors, tools/chd/make_fixture.py's
  Track 2  AUDIO       its two seconds of pregap held in its file (INDEX 00),
                       FLAGS DCP, as many Redump audio tracks have
  Track 3  AUDIO       a PREGAP its file does not hold
  SBI      two LibCrypt-style Q replacements in track 1

An ORE holds one file a track, so unlike the CHD fixture each track has
its own BIN.

Usage: make_fixture.py out_basename
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'chd'))
from make_fixture import (SECTOR, DATA_FRAMES, Lcg, audio_frames, bcd,
                          data_sector)

SBI_LBAS = (300, 301)


def main():
    if len(sys.argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    base = sys.argv[1]
    name = os.path.basename(base)

    rng = Lcg(0x5eed)
    with open(base + ' (Track 1).bin', 'wb') as f:
        for lba in range(DATA_FRAMES):
            f.write(data_sector(lba, rng, 0))
    pcm = audio_frames(Lcg(0xa0d10))
    with open(base + ' (Track 2).bin', 'wb') as f:
        f.write(pcm)
    with open(base + ' (Track 3).bin', 'wb') as f:
        f.write(pcm[:300 * SECTOR])

    with open(base + '.cue', 'w') as f:
        f.write('FILE "%s (Track 1).bin" BINARY\n' % name)
        f.write('  TRACK 01 MODE2/2352\n')
        f.write('    INDEX 01 00:00:00\n')
        f.write('FILE "%s (Track 2).bin" BINARY\n' % name)
        f.write('  TRACK 02 AUDIO\n')
        f.write('    FLAGS DCP\n')
        f.write('    INDEX 00 00:00:00\n')
        f.write('    INDEX 01 00:02:00\n')
        f.write('FILE "%s (Track 3).bin" BINARY\n' % name)
        f.write('  TRACK 03 AUDIO\n')
        f.write('    PREGAP 00:02:00\n')
        f.write('    INDEX 01 00:00:00\n')

    with open(base + '.sbi', 'wb') as f:
        f.write(b'SBI\0')
        for lba in SBI_LBAS:
            aba = lba + 150
            amsf = bytes((bcd(aba // 4500), bcd((aba // 75) % 60),
                          bcd(aba % 75)))
            rel = lba
            rmsf = bytes((bcd(rel // 4500), bcd((rel // 75) % 60),
                          bcd(rel % 75) ^ 0x01))
            # Type 1: the whole Q but its CRC, its relative frame altered
            f.write(amsf + b'\x01' + b'\x41\x01\x01' + rmsf + b'\x00'
                    + amsf)
    return 0


if __name__ == '__main__':
    sys.exit(main())
