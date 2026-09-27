#pragma once
#include <cstddef>
#include <cstdint>

// A track's length, estimated from how fast the decoder goes through its
// file: the bytes it read since its first audio, per frame it made, over
// the bytes still to read. The files carry no length the decoders expose
// (ESP8266Audio gives neither an MP3's frame count nor a FLAC's
// STREAMINFO), but for a constant bitrate MP3 this is exact, and for a
// variable one or a FLAC it settles within a few seconds. Measuring from
// the first audio leaves out the tags in front (an ID3 tag with a cover can
// be half a megabyte). Portable, host-tested.
namespace progress {

// `frames` made since the start at `rate` Hz; the file position when the
// first audio came (`pos0`), now (`pos`), and the file's size. 0: not
// enough to go on yet (under `minFrames`, or nothing read since pos0).
uint32_t estimateDurationMs(uint64_t frames, int rate, uint32_t pos0, uint32_t pos, uint32_t size,
                            uint64_t minFrames = 44100);

// The estimate is exact only for a constant bitrate: a VBR MP3 whose start
// is quieter than its average (most LAME VBR files) read 7:37 for a 5:20
// track 15 s in (on the device). Those files, and FLAC files, say their
// length at the start instead, which the backend reads when a track opens:

// The size of the ID3v2 tag the file starts with (its header, body and any
// footer), from its first 10 bytes; 0 if it has none.
uint32_t id3v2Size(const uint8_t* head, size_t n);

// An MP3's length from the Xing/Info or VBRI header in its first frame:
// `buf` holds the bytes after the ID3v2 tag. 0 when it has none (a plain
// constant bitrate file: estimateDurationMs() is exact for those).
uint32_t mp3HeaderDurationMs(const uint8_t* buf, size_t n);

// A FLAC file's length from its STREAMINFO (the first metadata block, right
// after "fLaC"): `buf` holds the file's first bytes (42 are enough). 0 if
// it isn't one, or the encoder didn't know the total.
uint32_t flacDurationMs(const uint8_t* buf, size_t n);

}  // namespace progress
