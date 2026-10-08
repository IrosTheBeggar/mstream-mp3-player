#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 IrosTheBeggar
"""synthcard.py: a synthetic big card for the Core2 (docs/METADATA.md 6.1,
row N11; 6.3, L0 and C2).

Builds, in a local folder, a card tree in the shape measured on the user's
library (the metascan research: aggregates only, no names): about 20,000
tiny MP3, FLAC and Opus stubs under /music in 705 artist folders, with the
measured album sizes, folder depths, file-name shapes, tag presence, the
tag/path disagreement rates, compilations, sort and multi-valued tags, some
embedded pictures and cover images, and the other files a real library
carries (.cue, .log, .m3u, hidden files). Every name is made up: artist
names from invented syllables, titles and albums from common English words,
genres from generic genre words. The audio is silence: a second or so of
valid frames (MPEG-1 Layer III at 32 kbps, FLAC CONSTANT subframes, Opus
CELT silence packets), so every stub plays, benches and parses.

Deterministic: the same --seed and --count give the same tree, byte for
byte, file times included (the plan draws from one random.Random(seed); a
file's bytes depend only on the plan).

Usage:

    python tools/synthcard.py --plan
        the summary of the default tree (seed 1, 19,519 audio files): counts
        by shape, the rates against the measured ones, the size on a FAT32
        card. Nothing is written.

    python tools/synthcard.py [--out DIR] [--count N] [--seed S]
        writes the tree into DIR (default: $SYNTHCARD_OUT, else
        <the system temp folder>/synthcard): DIR/music/..., DIR/SYNTHCARD.TXT
        (what the card is), DIR/synthcard.json (the summary, the device's
        expected counts, the probe albums for L0's open bench) and
        DIR/synthcard-files.jsonl (each file's tags as written, its length,
        its picture's anchor: a tag reader's expected dump). DIR must be a
        local folder (not a removable drive), empty or a tree this tool
        wrote (it is rebuilt).

    python tools/synthcard.py --out DIR --copy-to E:\\
        copies DIR's tree (music\\ and SYNTHCARD.TXT) to the root of the card
        in drive E:. It first prints the drive's label, size, free space and
        file system and what it will copy, and copies only with --yes. It
        refuses unless the drive is removable, FAT32 (the firmware reads no
        other: README), not the system drive, empty (Windows' own System
        Volume Information aside) and has room. It never deletes, overwrites
        or formats anything. The artist folders are created in the order the
        summary's probes assume (NTFS's name order, as Windows lists a
        folder), with a progress line. Windows only.

The summary's probes: three plain MP3 albums (no pictures, no covers) whose
artist folders sit at the entries of /music, in creation order, nearest 1,
353 and 705 that hold such an album (1, 353 and 703 in the default tree:
its last two entries are the made-up Katakana names), so the open time by
a folder's place in a 705-entry directory (L0's check) can be read from
plays of the three (the runbook's start latency).

Its tests: python -m unittest discover -s tools -p "test_synthcard.py"
(the shape statistics; the stubs parsed by ffprobe when it is installed,
else by the repo's own parsers through tools/synthcard_probe.cpp, built
with the host's g++).
"""
import argparse
import base64
import bisect
import json
import math
import os
import random
import shutil
import struct
import sys
import tempfile
import time
import unicodedata
import zlib

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):
    pass

# ---------------------------------------------------------------------------
# The measured shape (metascan: cost/structure.out, exists/paths*.out,
# datamodel/dbstats.out; RESEARCH.md section 4). Per 19,519 audio files.
# ---------------------------------------------------------------------------

MEASURED = {
    "audio_files": 19519,
    "mp3": 18263,
    "flac": 1147,
    "other_audio": {"wma": 68, "m4a": 30, "mp4": 10, "ogg": 1},
    "artists": 705,
    "music_fat_entries": 1628,
    "music_sectors": 102,
    "folders": 2591,
    "audio_folders": 1791,
    "depth": {1: 36, 2: 18261, 3: 1214, 4: 8},
    "per_folder": {"mean": 10.9, "p50": 11, "p90": 17, "p99": 28, "max": 43},
    "dir_entries": {"mean": 37.95, "p50": 35, "p90": 73, "p99": 124},
    "open_sectors_mean": 56.2,
    "open_sectors_music": 51.3,
    "images": 1716,
    "other_files": 1342,
    "hidden": 63,
    "va_files": 338,
    "disc_subfolder_albums": 30,
    "sibling_disc_sets": 8,
    "restart_folders": 24,
    "disc_track_names": 224,
    "three_digit_names": 143,
    "no_number_folders": 178,
}

# The rates, in percent (of tracks unless said). The same constants as N2's
# lib/core/LibrarySynth.h where it has them (kTitleSamePct ...).
RATES = {
    # The tag title against the file name's title (of tracks).
    "title": {"same": 61, "artist": 22, "inside": 6, "case": 4, "longer": 2, "other": 1, "none": 0.6},
    # The album folder against the album tag (of album folders).
    "album": {"same": 38, "year": 35, "suffix": 16, "other": 4, "case": 7},
    # The artist folder against the album artist, else the artist (of artists).
    "artist": {"same": 88, "case": 9, "other": 3},
    # Presence (of tracks; album-level ones drawn per album).
    "track": 98, "year": 95, "genre_mp3": 82, "genre_flac": 52, "disc": 22, "disc_multi": 35,
    "albumartist_mp3": 30, "compilation_other": 0.3,
    "albumartist_flac": 76, "compilation_va": 45, "sort_artists": 19, "multi_artist": 4, "feat_title": 6,
    "picture_mp3": 22, "picture_flac": 47, "picture_opus": 30, "rg": 2.5, "bpm": 0.9, "mbid": 1.4,
    "no_tags": 0.3, "title_none": 0.6, "album_none": 0.4,
    # The MP3 tag containers (of MP3 albums).
    "id3": {"2.3": 80, "2.4": 17, "2.2": 3}, "id3v1": 60, "id3v1_only": 2, "ape": 0.3, "unsync_v24": 55,
    "xing": 76, "lame": 76, "tlen": 15, "comm": 15, "pic_first": 5,
}

# The shapes of a file's name (of album folders; all of an album's files
# follow its shape). `None`: the name carries no number.
NAME_SHAPES = [
    ("NN - Title", 40.0), ("NN Title", 15.0), ("NN. Title", 5.0), ("NN-Title", 2.0), ("NN_title", 1.0),
    ("NN - Artist - Title", 10.0), ("NN Artist - Title", 3.5), ("Artist - NN - Title", 4.0),
    ("Artist - Album - NN - Title", 2.5), ("(NN) Title", 0.4), ("Title", 7.5), ("Artist - Title", 2.0),
]
ARTIST_SHAPES = {"NN - Artist - Title", "NN Artist - Title", "Artist - NN - Title", "Artist - Album - NN - Title",
                 "Artist - Title"}
NUMBERLESS = {"Title", "Artist - Title"}

# Tracks per album folder: weights for n tracks (the measured p50 11, p90 17,
# p99 28, max 43; 104 single-file folders in 1,791).
TRACK_WEIGHTS = {1: 68, 2: 14, 3: 16, 4: 20, 5: 24, 6: 30, 7: 42, 8: 64, 9: 90, 10: 118, 11: 118, 12: 112,
                 13: 86, 14: 62, 15: 45, 16: 33, 17: 24, 18: 18, 19: 13, 20: 9, 21: 7, 22: 6, 23: 5, 24: 4,
                 25: 3, 26: 3, 27: 2, 28: 2, 29: 1.5, 30: 1.5, 32: 1, 34: 1, 36: 0.7, 38: 0.5, 40: 0.4, 43: 0.3}

# Albums per artist (705 artists, 1,791 audio folders): weights.
ALBUMS_PER_ARTIST = {1: 50, 2: 18, 3: 10, 4: 6, 5: 4, 6: 3, 7: 2, 8: 2, 9: 1.2, 10: 1, 12: 0.8, 14: 0.6, 16: 0.5,
                     20: 0.4, 25: 0.25, 32: 0.15, 40: 0.1}

PROBE_POSITIONS = (1, 353, 705)  # 1-based entries of /music, in creation order

# ---------------------------------------------------------------------------
# Words. Made-up syllables for people and bands; common words for the rest.
# ---------------------------------------------------------------------------

SYLLABLES = ("ba bel bri cal cor dal del dra el en fa fen gal gor ha hel ir is ja ka kel kor la lin lo lu ma mer "
             "mir mo na nel nor ol os pa pel quin ra rel ro ru sa sel sil so ta tel tor tu va vel vin vor wa wel "
             "xan ya yel za zel zor ost rin mon ver lis tam bro dun fel gri hal jor kip lem mav nim pry quo res "
             "sul tiv ulm vex wyn yar zed").split()
WORDS = ("glass river morning silver paper empty golden quiet broken northern electric hollow velvet distant "
         "wild open slow little bright cold lonely secret burning falling hidden last early late blue green red "
         "black white grey amber violet crimson heavy light soft long short deep high low new old young "
         "summer winter autumn spring night day evening dawn dusk midnight noon city street road harbour "
         "garden house window door room wall bridge tower station field forest mountain valley ocean island "
         "shore desert canyon meadow lake rain snow storm thunder wind cloud sky star moon sun fire smoke ash "
         "stone iron steel gold copper rust dust sand salt honey sugar wine coffee tea bread milk letter song "
         "story dream memory shadow mirror circle line signal echo voice heart hand eye mind soul ghost angel "
         "stranger lover friend sister brother mother father child king queen sailor soldier dancer driver "
         "runner singer thief doctor teacher machine engine radio television camera telephone motor wheel "
         "rocket satellite planet comet orbit galaxy atlas compass map anchor arrow ribbon feather flower "
         "rose lily orchid ivy pine oak willow maple cedar birch apple cherry lemon orange plum tiger wolf fox "
         "bear horse rabbit raven crow swan heron sparrow falcon whale shark dolphin spider bee butterfly "
         "moth return escape arrival departure holiday weekend tomorrow yesterday forever always never "
         "somewhere nowhere everywhere inside outside between beyond above below under over through away "
         "home alone together again still almost only maybe").split()
NOUNS = ("river morning house window door room bridge tower station field forest mountain valley ocean island "
         "shore desert lake rain snow storm thunder wind cloud sky star moon sun fire stone iron gold letter "
         "song story dream memory shadow mirror circle signal echo voice heart hand ghost stranger friend "
         "machine engine radio camera planet comet atlas compass anchor arrow feather flower rose tiger wolf "
         "fox raven crow swan heron falcon whale holiday harbour garden street road city").split()
ADJECTIVES = ("glass silver paper empty golden quiet broken northern electric hollow velvet distant wild open "
              "slow little bright cold lonely secret burning falling hidden last early late blue green red black "
              "white grey amber violet crimson heavy soft long deep high new old young").split()
BAND_NOUNS = ("Trio Quartet Ensemble Collective Orchestra Band Project Sound Society Club Brigade Choir "
              "Experience Machine Unit Union Players Brothers Sisters Kids Family").split()
SUFFIXES = ("(Live)", "(Remix)", "(Acoustic)", "(Demo)", "(Radio Edit)", "(Instrumental)", "(Reprise)",
            "(Extended Mix)", "(Bonus Track)", "[Live]", "(Interlude)", "(Version 2)")
LONGER = ("(Remastered)", "(2011 Remaster)", "(Album Version)", "(Single Version)", "(Mono)", "(Stereo Mix)")
EDITIONS = ("(Deluxe Edition)", "(Remastered)", "(Expanded Edition)", "(Special Edition)", "(Bonus Tracks)",
            "(EP)", "(Single)", "(Anniversary Edition)")
FORMAT_TAGS = ("[FLAC]", "[MP3]", "(320)", "[V0]", "[320]", "[WEB]", "(CD)", "[16-44]", "[24-96]", "(MP3 V0)")

ID3V1_GENRES = (
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop", "Jazz", "Metal", "New Age",
    "Oldies", "Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno", "Industrial", "Alternative", "Ska",
    "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop", "Vocal", "Jazz+Funk", "Fusion",
    "Trance", "Classical", "Instrumental", "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise",
    "AlternRock", "Bass", "Soul", "Punk", "Space", "Meditative", "Instrumental Pop", "Instrumental Rock",
    "Ethnic", "Gothic", "Darkwave", "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream",
    "Southern Rock", "Comedy", "Cult", "Gangsta", "Top 40", "Christian Rap", "Pop/Funk", "Jungle",
    "Native American", "Cabaret", "New Wave", "Psychadelic", "Rave", "Showtunes", "Trailer", "Lo-Fi", "Tribal",
    "Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll", "Hard Rock")
GENRE_WORDS = ("Indie Rock", "Indie Pop", "Post-Rock", "Synthpop", "Deep House", "Downtempo", "Shoegaze",
               "Dream Pop", "Neo-Soul", "Drum and Bass", "Dubstep", "Garage", "Breakbeat", "IDM", "Glitch",
               "Minimal", "Krautrock", "Afrobeat", "Bossa Nova", "Latin", "Flamenco", "Celtic", "Bluegrass",
               "Americana", "Alt-Country", "Emo", "Hardcore", "Math Rock", "Noise Rock", "Stoner Rock", "Doom",
               "Black Metal", "Thrash", "Power Metal", "Prog", "Art Rock", "Chamber Pop", "Baroque Pop",
               "Electro", "Nu Disco", "Italo", "Synthwave", "Chillwave", "Lounge", "Exotica", "Swing", "Bebop",
               "Cool Jazz", "Free Jazz", "Soul Jazz", "Funk Rock", "Garage Rock", "Surf", "Rockabilly",
               "Psychedelic Rock", "Folk Rock", "Singer-Songwriter", "Chanson", "Bolero", "Cumbia", "Salsa",
               "Tango", "Fado", "Highlife", "Dub", "Dancehall", "Ska Punk", "Grime", "UK Garage", "Footwork",
               "Ambient Techno", "Acid House", "Minimal Techno", "Dark Ambient", "Drone", "Modern Classical",
               "Baroque", "Opera", "Choral", "Film Score", "Video Game", "Spoken Word", "Children's",
               "Christmas", "World", "Electronica", "Big Beat", "Trip Hop", "Lo-fi Hip Hop", "Boom Bap",
               "Jazz Rap", "Trap", "Cloud Rap", "Hyperpop", "Post-Punk", "Coldwave", "Darkwave Revival",
               "Gothic Rock", "Industrial Rock", "Nu Metal", "Metalcore", "Sludge", "Post-Metal", "Blackgaze",
               "Slowcore", "Sadcore", "Twee", "Jangle Pop", "Power Pop", "Britpop", "Madchester", "Baggy",
               "Space Rock", "Kosmische", "Library Music", "Easy Listening", "New Romantic", "Freestyle",
               "Hi-NRG", "Eurobeat", "Vaporwave", "Future Garage", "Liquid Funk", "Jump Up", "Neurofunk",
               "Hardstyle", "Gabber", "Happy Hardcore", "Psytrance", "Goa", "Progressive House", "Tech House",
               "Electro House", "Microhouse", "Balearic", "Yacht Rock", "Soft Rock", "AOR", "Arena Rock",
               "Glam Rock", "Pub Rock", "Proto-Punk", "Hardcore Punk", "Pop Punk", "Skate Punk", "Crust",
               "Grindcore", "Death Industrial", "Power Electronics", "Musique Concrete", "Tape Music",
               "Field Recordings", "New Age Ambient", "Meditation", "Throat Singing", "Gamelan", "Qawwali",
               "Rai", "Mbalax", "Soukous", "Kwaito", "Amapiano", "Gqom", "Baile Funk", "Tropicalia", "MPB",
               "Samba", "Forro", "Axe", "Son", "Mambo", "Merengue", "Bachata", "Reggaeton", "Corrido",
               "Ranchera", "Norteno", "Zydeco", "Cajun", "Western Swing", "Honky Tonk", "Outlaw Country",
               "Country Pop", "Gospel Blues", "Delta Blues", "Chicago Blues", "Electric Blues", "Jump Blues",
               "Doo-Wop", "Northern Soul", "Southern Soul", "Philly Soul", "Quiet Storm", "New Jack Swing",
               "Contemporary R&B", "Alternative R&B", "G-Funk", "Horrorcore", "Chopped and Screwed",
               "Crunk", "Bounce", "Drill", "Phonk", "Plunderphonics", "Breakcore", "Witch House")

ACCENTS = {"a": "áàâä", "e": "éèêë", "i": "íï", "o": "óöô", "u": "úü", "n": "ñ", "c": "ç"}
KATAKANA = [chr(c) for c in range(0x30A2, 0x30F3) if chr(c) not in "ヵヶ"]
HIRAGANA = [chr(c) for c in range(0x3042, 0x3093)]
CYRILLIC = [chr(c) for c in range(0x0430, 0x0450)]
GREEK = [chr(c) for c in range(0x03B1, 0x03CA) if c != 0x03C2]
WIN_RESERVED = {"CON", "PRN", "AUX", "NUL"} | {f"COM{i}" for i in range(1, 10)} | {f"LPT{i}" for i in range(1, 10)}
FAT_BAD = '/\\:*?"<>|'

# Two pictures: 48x48 JPEGs (baseline and progressive), the bytes of PIL 12
# (quality 60): a cover has to decode, not to look like anything.
JPEG_BASELINE = base64.b64decode(
    "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAA0JCgsKCA0LCgsODg0PEyAVExISEyccHhcgLikxMC4pLSwzOko+MzZGNywtQFdBRkxOUlNSMj5aYV"
    "pQYEpRUk//2wBDAQ4ODhMREyYVFSZPNS01T09PT09PT09PT09PT09PT09PT09PT09PT09PT09PT09PT09PT09PT09PT09PT09PT0//wAARCAAw"
    "ADADASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhBy"
    "JxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKT"
    "lJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAA"
    "AAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRom"
    "JygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExc"
    "bHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDGoors6eIxHsbaXuTTp899TjKK7Oiub+0P7v4/8A1+reZxlF"
    "dnXJXv/H9cf9dG/nXRh8T7ZtWsZ1KXItyGuzrjK7OufMPs/P8AQ0w3U4XWtYm1C4eONytqpwqqSA4z1Pr0/CoNM1S402YNExaLPzRE/K3+B46"
    "0atp0um3jRsp8piTE/Xcv+PrUFlZzX1ysFuu5j1J6KPU+1dEY0/Z2WxLcubzPRYpEmiSWM5R1DKcdQelcre/8f1x/10b+ddTBEsFvHChJWNAgJ6"
    "4AxXLXv/H9cf8AXRv51zYC3PKxeI2RDXZ1xlTfbLr/AJ+Zv++zXTicO61rPYypVFC51Uscc0ZjlRXQ9VYZB/CkhghgQpBEkSk5IRQoz+Fct9suv+"
    "fmb/vs0fbLr/n5m/77Nc31Cdrcxr9YXY62uSvf+P64/wCujfzo+2XX/PzN/wB9momJZizEkk5JPeujDYZ0W22ZVaqmj//Z")
JPEG_PROGRESSIVE = base64.b64decode(
    "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAA0JCgsKCA0LCgsODg0PEyAVExISEyccHhcgLikxMC4pLSwzOko+MzZGNywtQFdBRkxOUlNSMj5aYV"
    "pQYEpRUk//2wBDAQ4ODhMREyYVFSZPNS01T09PT09PT09PT09PT09PT09PT09PT09PT09PT09PT09PT09PT09PT09PT09PT09PT0//wgARCAAw"
    "ADADASIAAhEBAxEB/8QAGQABAQEAAwAAAAAAAAAAAAAABQACAwQG/8QAFwEBAQEBAAAAAAAAAAAAAAAAAwIBAP/aAAwDAQACEAMQAAAB6VNdgs"
    "1HQs0Qk4aFaOguOwmeiKVKKsNC7WFYqK1yLKR//8QAHBAAAgIDAQEAAAAAAAAAAAAAAQIDEAASMhEw/9oACAEBAAEFAvm/dTTGRo5WjIPofupY"
    "zG6IXYDxX7ogEAAU/ebtm7Zu2btX/8QAGREAAwEBAQAAAAAAAAAAAAAAAAIRAxMh/9oACAEDAQE/AVWnMZYZntNBWh0Gan//xAAfEQACAgAHAQ"
    "AAAAAAAAAAAAAAAwESAgQTFDFBUWH/2gAIAQIBAT8BYyhuPgtlzMdERhqI5kYu5oT6LXQ//8QAHxAAAQQCAwEBAAAAAAAAAAAAAAECEBEycSFB"
    "UTFx/9oACAEBAAY/Avm7c0mJx+eFoO3NddFNETwduaU4SoduMlMlMlMlj//EACAQAQACAQQCAwAAAAAAAAAAAAEAERAxQaHxIVEwYbH/2gAIAQ"
    "EAAT8h+PlMtEo6BvLmr3bGAWglk5TLJHlq9kDFb+QidBU5TNCBPTD6IfRWOUx2s7WdrO1j5bZ//9oADAMBAAIAAwAAABDrHWlna25v/8QAGxEA"
    "AgIDAQAAAAAAAAAAAAAAAAERITFBcVH/2gAIAQMBAT8Qs2dkPJuN9DBCpkPBan//xAAbEQADAQADAQAAAAAAAAAAAAAAAREhMcHh8P/aAAgBAg"
    "EBPxDBlp8XwqakOzowFwTkOiPgzgezbZ//xAAhEAEAAQMEAgMAAAAAAAAAAAABEQAxURCB8PEhMEFh4f/aAAgBAQABPxD18hnVBKIQgZu5ttRB"
    "Uvleh8XprpMkXG1chnVYEy3/ANM1MULrYZfqnRUJW8BFchnVgt3CR2pW/MoQnbTkM6dlrstdlrstJRFVlX5r/9k=")


def png_bytes(w=8, h=8, rgb=(90, 120, 160)):
    """A tiny truecolour PNG (zlib, no filter)."""
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + bytes(rgb) * w for _ in range(h))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


PNG_SMALL = png_bytes()

# ---------------------------------------------------------------------------
# Checksums (table-driven: about 20,000 files)
# ---------------------------------------------------------------------------


def _msb_table(poly, width):
    top, mask = 1 << (width - 1), (1 << width) - 1
    t = []
    for b in range(256):
        c = b << (width - 8)
        for _ in range(8):
            c = ((c << 1) ^ poly) & mask if c & top else (c << 1) & mask
        t.append(c)
    return t


_OGG = _msb_table(0x04C11DB7, 32)
_CRC8 = _msb_table(0x07, 8)
_CRC16 = _msb_table(0x8005, 16)


def ogg_crc(data):
    c = 0
    for b in data:
        c = ((c << 8) & 0xFFFFFFFF) ^ _OGG[((c >> 24) ^ b) & 0xFF]
    return c


def crc8(data):
    c = 0
    for b in data:
        c = _CRC8[c ^ b]
    return c


def crc16(data):
    c = 0
    for b in data:
        c = ((c << 8) & 0xFFFF) ^ _CRC16[((c >> 8) ^ b) & 0xFF]
    return c


def crc16_lame(data):
    """CRC-16/ARC, as LAME's tag (lib/core/LameTag.cpp crc16())."""
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return c


def syncsafe(n):
    return bytes([(n >> 21) & 0x7F, (n >> 14) & 0x7F, (n >> 7) & 0x7F, n & 0x7F])


def unsync(b):
    """ID3 unsynchronisation: a 00 after every FF that a 00 or E0-FF follows, or that ends the data."""
    out = bytearray()
    n = len(b)
    for i, x in enumerate(b):
        out.append(x)
        if x == 0xFF and (i + 1 == n or b[i + 1] == 0 or b[i + 1] >= 0xE0):
            out.append(0)
    return bytes(out)


# ---------------------------------------------------------------------------
# Names
# ---------------------------------------------------------------------------


def fold(s):
    """A case- and accent-insensitive key (textfold::compare's spirit)."""
    d = unicodedata.normalize("NFKD", s)
    return "".join(c for c in d if not unicodedata.combining(c)).casefold()


def ntfs_key(s):
    """NTFS's name order (as Windows lists a folder; not Explorer's view, which
    sorts numbers by value): by the upper-cased UTF-16 units."""
    return s.upper().encode("utf-16-be")


def fat_safe(s):
    """A name as the card stores it: FAT's forbidden characters replaced, no
    trailing dots or spaces, not a Windows device name."""
    out = []
    for c in s:
        if c in FAT_BAD or ord(c) < 32:
            out.append({"?": "", ":": " -", "/": "-", '"': "'", "*": "", "<": "", ">": "", "|": "-"}.get(c, "_"))
        else:
            out.append(c)
    r = "".join(out).rstrip(" .")
    r = " ".join(r.split())
    if not r:
        r = "_"
    if r.split(".")[0].upper() in WIN_RESERVED:
        r = "_" + r
    return r


class Names:
    def __init__(self, rng):
        self.rng = rng

    def word(self, n=None):
        r = self.rng
        n = n or r.choice((2, 2, 2, 3, 3, 1))
        w = "".join(r.choice(SYLLABLES) for _ in range(n))
        return w[:1].upper() + w[1:]

    def accent(self, w):
        r = self.rng
        idx = [i for i, c in enumerate(w) if c.lower() in ACCENTS]
        if not idx:
            return w
        i = r.choice(idx)
        a = r.choice(ACCENTS[w[i].lower()])
        return w[:i] + (a.upper() if w[i].isupper() else a) + w[i + 1:]

    def script(self, alphabet, n):
        return "".join(self.rng.choice(alphabet) for _ in range(n))

    def hangul(self, n):
        return "".join(chr(0xAC00 + self.rng.randrange(11172)) for _ in range(n))

    def artist(self):
        r = self.rng
        k = r.random() * 100
        if k < 45:
            return f"{self.word()} {self.word()}"
        if k < 64:
            return self.word(r.choice((2, 3)))
        if k < 72:
            return f"The {self.word()}s"
        if k < 75:
            return f"{self.word()} & the {self.word()}s"
        if k < 79:
            return f"{self.word()} {self.word()} & {self.word()} {self.word()}"
        if k < 87:
            return f"{self.word()} {r.choice(BAND_NOUNS)}"
        if k < 89:
            return f"{r.choice(('DJ', 'MC'))} {self.word()}"
        if k < 95:
            return f"{self.word()} {self.word()} {self.word(1)}"
        if k < 97:
            return r.choice((f"{self.word()} {r.randrange(2, 100)}", f"{r.randrange(2, 13)} {self.word()}s"))
        if k < 98:
            return self.word().lower()
        return f"{self.accent(self.word())} {self.accent(self.word())}"

    def phrase(self, lo=1, hi=5):
        r = self.rng
        k = r.random()
        if k < 0.26:
            p = f"{r.choice(ADJECTIVES)} {r.choice(NOUNS)}"
        elif k < 0.40:
            p = f"{r.choice(NOUNS)} of the {r.choice(ADJECTIVES)} {r.choice(NOUNS)}" if r.random() < 0.4 else                 f"{r.choice(NOUNS)} of {r.choice(NOUNS)}"
        elif k < 0.52:
            p = f"the {r.choice(ADJECTIVES)} {r.choice(NOUNS)}"
        elif k < 0.60:
            p = r.choice(NOUNS)
        elif k < 0.78:
            p = " ".join(r.choice(WORDS) for _ in range(r.randint(lo + 1, hi)))
        elif k < 0.89:
            p = f"{r.choice(WORDS)} {r.choice(('and', 'in', 'on', 'for', 'to', 'with'))} the {r.choice(WORDS)}"
        elif k < 0.95:
            p = f"{r.choice(NOUNS)}, {r.choice(NOUNS)} & {r.choice(NOUNS)}"
        else:
            p = f"{r.choice(ADJECTIVES)} {r.choice(NOUNS)} {r.choice(('Part', 'No.', 'Vol.'))} {r.randint(1, 9)}"
        return " ".join(w[:1].upper() + w[1:] for w in p.split(" "))

    def title(self):
        r = self.rng
        k = r.random() * 100
        t = self.phrase()
        if k < 2.3:  # a title starting with a digit (450 of 19,371)
            t = f"{r.choice((2, 4, 7, 12, 99, 1999, 2000, 24))} {t}"
        elif k < 3.3:  # accented (title_non_ascii 189: 1 %)
            t = self.accent(t)
        elif k < 3.6:  # non-Latin (0.3 %): made-up strings in other scripts
            pick = r.random()
            if pick < 0.45:
                t = self.hangul(r.randint(2, 6))
            elif pick < 0.7:
                t = self.script(KATAKANA, r.randint(3, 7))
            elif pick < 0.85:
                t = self.script(HIRAGANA, r.randint(3, 6))
            else:
                t = self.script(CYRILLIC, r.randint(4, 9)).capitalize()
        elif k < 4.6:  # typographic apostrophes and dashes
            t = t.replace(" ", "\u2019s ", 1) if " " in t else t + "\u2019s"
        elif k < 5.4:  # a character FAT can't store, in the tag
            t = t + r.choice(("?", "?", ": Part Two", " / Reprise", ' "Live"', " *"))
        return t

    def album(self, artist_tag):
        r = self.rng
        k = r.random() * 100
        if k < 5:
            return artist_tag  # self-titled
        a = self.phrase()
        if k < 6:
            a = self.accent(a)
        elif k < 6.3:
            a = self.script(r.choice((KATAKANA, GREEK)), r.randint(3, 6))
        elif k < 7.3:
            a = a + r.choice(("?", ": Act One", " / Remixes"))
        elif k < 15:
            a = f"The {a}" if not a.startswith("The ") else a
        return a


# ---------------------------------------------------------------------------
# The plan: what the card holds, with no bytes yet
# ---------------------------------------------------------------------------


def wpick(rng, weighted):
    """weighted: [(value, weight)] or {value: weight}."""
    items = list(weighted.items()) if isinstance(weighted, dict) else list(weighted)
    total = sum(w for _, w in items)
    x = rng.random() * total
    for v, w in items:
        x -= w
        if x < 0:
            return v
    return items[-1][0]


def pct(rng, p):
    return rng.random() * 100 < p


def scaled(n, s, lo=0):
    return max(lo, int(round(n * s)))


class Plan:
    """Artists (each a dict), their albums (each a dict with its folders and
    tracks), the other files, the folders; summary() counts them."""

    def __init__(self, count, seed, opus_share):
        self.count, self.seed, self.opus_share = count, seed, opus_share
        self.artists = []
        self.files = []     # every file: dicts with rel, kind, ...
        self.folders = []   # every folder (rel, no trailing slash), "music" first


def _album_tracks(rng):
    return wpick(rng, TRACK_WEIGHTS)


def make_plan(count=MEASURED["audio_files"], seed=1, opus_share=1.0):
    rng = random.Random(seed)
    names = Names(rng)
    s = count / MEASURED["audio_files"]
    plan = Plan(count, seed, opus_share)
    genres = list(ID3V1_GENRES) + list(GENRE_WORDS)
    genre_w = [(g, 1.0 / (i + 1) ** 1.1) for i, g in enumerate(rng.sample(genres, len(genres)))]

    # -- the artists --
    n_artists = scaled(MEASURED["artists"], s, 1)
    seen = set()
    artists = []
    n_va = 2 if n_artists >= 40 else 0
    while len(artists) < n_artists - n_va:
        tag = names.artist()
        folder = fat_safe(tag)
        if fold(folder) in seen or fold(tag) in seen or len(folder) > 60:
            continue
        seen.add(fold(folder))
        artists.append({"tag": tag, "folder": folder, "va": False, "albums": [], "loose": [], "rel": "same"})
    # Two artists with made-up Katakana names, when the card is big enough.
    for i in range(min(2, len(artists) // 300)):
        a = artists[i * 7 + 3]
        a["tag"] = a["folder"] = names.script(KATAKANA, rng.randint(3, 6))
        seen.add(fold(a["folder"]))
    for label in ("Various Artists", "VA")[:n_va]:
        artists.append({"tag": "Various Artists", "folder": label, "va": True, "albums": [], "loose": [],
                        "rel": "same"})
    # The artist folder against the tag (88 / 9 / 3), and sort tags on a share of "The" bands.
    the_bands = [a for a in artists if a["tag"].startswith("The ") and not a["va"]]
    for a in the_bands[:scaled(RATES["sort_artists"], s)]:
        a["sort"] = a["tag"][4:] + ", The"
    for a in artists:
        if a["va"]:
            continue
        rel = wpick(rng, RATES["artist"])
        a["rel"] = rel
        if rel == "case":
            k = rng.random()
            if k < 0.35:
                a["folder"] = a["folder"].lower() if a["folder"] != a["folder"].lower() else a["folder"].upper()
            elif k < 0.6:
                # a FAT-unsafe character in the tag, replaced in the folder ("Nox/Vale" in Nox-Vale)
                ch = rng.choice(("/", ":", "?", "*", '"'))
                parts = a["tag"].split(" ", 1)
                if ch == "/" and len(parts) > 1:
                    a["tag"] = parts[0] + "/" + parts[1]
                elif ch == '"' and len(parts) > 1:
                    a["tag"] = f'{parts[0]} "{parts[1]}"'
                elif len(parts) > 1:
                    a["tag"] = parts[0] + ch + " " + parts[1]
                else:
                    a["tag"] = a["tag"] + ch
                a["folder"] = fat_safe(a["tag"])
            elif k < 0.8 and a["tag"].startswith("The "):
                a["folder"] = a["tag"][4:] + ", The"
            elif " & " in a["folder"]:
                a["folder"] = a["folder"].replace(" & ", " and ")
            elif " " in a["folder"]:
                a["folder"] = a["folder"].replace(" ", rng.choice(("_", "-", ".")), 1)
            else:
                a["folder"] = a["folder"].upper()
        elif rel == "other":
            a["tag"] = a["tag"] + " " + rng.choice(BAND_NOUNS)
    # Unique folders, case-insensitively (the card can't hold two that differ in case only).
    used = set()
    for a in artists:
        f = a["folder"]
        while fold(f) in used:
            f = f + "_"
        a["folder"] = f
        used.add(fold(f))
    plan.artists = artists
    normal = [a for a in artists if not a["va"]]
    vas = [a for a in artists if a["va"]]

    # -- the albums: the special shapes first, then plain ones to the count --
    albums = []

    def album(n, **kw):
        d = {"n": n, "layout": "flat", "discs": 1, "kind": "mp3", "names": None}
        d.update(kw)
        albums.append(d)
        return d

    total = 0
    if vas:
        va_target = scaled(MEASURED["va_files"], s)
        while total < va_target:
            n = min(max(8, _album_tracks(rng)), 24)
            album(n, va=True)
            total += n
    for _ in range(scaled(MEASURED["disc_subfolder_albums"], s)):
        discs = wpick(rng, {2: 70, 3: 20, 4: 10})
        per = [rng.randint(11, 23) for _ in range(discs)]
        album(sum(per), layout="subfolders", discs=discs, per_disc=per)
        total += sum(per)
    if s >= 0.2:
        # The deepest four levels: a box set's disc with a bonus folder in it.
        album(8, layout="deep", discs=1, per_disc=[8])
        total += 8
    for _ in range(scaled(MEASURED["sibling_disc_sets"], s)):
        per = [rng.randint(8, 16), rng.randint(6, 14)]
        album(sum(per), layout="siblings", discs=2, per_disc=per)
        total += sum(per)
    for _ in range(scaled(MEASURED["restart_folders"], s)):
        per = [rng.randint(8, 14), rng.randint(6, 12)]
        album(sum(per), layout="flat", discs=2, per_disc=per, names="restart")
        total += sum(per)
    dt_target = scaled(MEASURED["disc_track_names"], s)
    dt = 0
    while dt < dt_target:
        per = [rng.randint(8, 13), rng.randint(7, 12)]
        album(sum(per), layout="flat", discs=2, per_disc=per, names="D-NN Title")
        dt += sum(per)
        total += sum(per)
    td_target = scaled(MEASURED["three_digit_names"], s)
    td = 0
    while td < td_target:
        per = [rng.randint(8, 12), rng.randint(6, 10)]
        album(sum(per), layout="flat", discs=2, per_disc=per, names="DNN - Title")
        td += sum(per)
        total += sum(per)
    loose_n = scaled(MEASURED["depth"][1], s)
    total += loose_n
    other = {k: scaled(v, s) for k, v in MEASURED["other_audio"].items()}
    for ext, n in other.items():
        left = n
        while left > 0:
            k = min(left, rng.randint(8, 12)) if ext != "ogg" else 1
            album(k, kind=ext)
            left -= k
            total += k
    while total < count:
        n = min(_album_tracks(rng), count - total)
        album(n)
        total += n
    while total > count:  # (a small --count: the special shapes alone overshoot)
        a = albums[-1]
        cut = min(a["n"] - 1, total - count)
        if cut <= 0 or a.get("per_disc"):
            albums.pop()
            total -= a["n"]
            continue
        a["n"] -= cut
        total -= cut

    # Formats of the plain albums: FLAC (measured) and Opus (the --opus-share; the library has none).
    flac_target = scaled(MEASURED["flac"], s)
    opus_target = int(round(count * opus_share / 100.0))
    plain = [a for a in albums if a["kind"] == "mp3" and not a.get("va") and a["layout"] == "flat"
             and not a.get("names")]
    order = list(range(len(plain)))
    rng.shuffle(order)
    got_f = got_o = 0
    for i in order:
        a = plain[i]
        if got_f < flac_target and a["n"] <= flac_target - got_f + 6:
            a["kind"] = "flac"
            got_f += a["n"]
        elif got_o < opus_target and a["n"] <= opus_target - got_o + 6:
            a["kind"] = "opus"
            got_o += a["n"]
    for a in albums:
        if a["layout"] in ("subfolders", "siblings") and got_f < flac_target and pct(rng, 30):
            a["kind"] = "flac"
            got_f += a["n"]

    # -- albums to artists --
    rng.shuffle(albums)
    va_albums = [a for a in albums if a.get("va")]
    rest = [a for a in albums if not a.get("va")]
    for i, a in enumerate(va_albums):
        vas[i % len(vas)]["albums"].append(a)
    weights = [1.0 / (rng.random() ** 0.85 + 0.02) for _ in normal]
    cum = []
    acc = 0.0
    for w in weights:
        acc += w
        cum.append(acc)
    for i, a in enumerate(rest):
        if i < len(normal):
            normal[i]["albums"].append(a)
        else:
            normal[bisect.bisect_left(cum, rng.random() * acc)]["albums"].append(a)
    # Loose tracks at depth 1: in a dozen artist folders.
    loose_artists = rng.sample(normal, min(len(normal), max(1, scaled(12, s, 1)))) if loose_n else []
    for i in range(loose_n):
        loose_artists[i % len(loose_artists)]["loose"].append(i)

    # -- names, tags and files --
    plan.folders.append("music")
    years = list(range(1958, 2025))
    year_w = [(y, 1 + max(0, y - 1958) ** 1.3) for y in years]
    for a in artists:
        a["folder_rel"] = "music/" + a["folder"]
        plan.folders.append(a["folder_rel"])
        album_seen = set()
        for al in a["albums"]:
            _fill_album(plan, rng, names, a, al, album_seen, genre_w, year_w)
        if a["loose"]:
            _fill_loose(plan, rng, names, a, genre_w, year_w)
    _probes(plan, rng)
    _extras(plan, rng, names, s)
    plan.artists = artists
    return plan


def _album_folder_name(rng, a, al, tag_album, year):
    """The album folder, against the tag (38 same / 35 year / 16 suffix / 4 other / 7 case)."""
    rel = wpick(rng, RATES["album"])
    base = fat_safe(tag_album)
    if base != tag_album and rel == "same":
        rel = "case"  # the card can't store it as tagged
    prefix = f"{a['folder']} - " if rel in ("year", "suffix") and pct(rng, 40) and not a["va"] else ""
    if rel == "same":
        name = base
    elif rel == "year":
        form = wpick(rng, {"({y})": 55, "{y} - ": 15, "[{y}]": 15, "[{y}] ": 5, " - {y}": 10})
        if form.endswith(" - ") or form == "[{y}] ":
            name = prefix + form.format(y=year) + base
        else:
            name = f"{prefix}{base} {form.format(y=year)}".replace("  ", " ")
        if pct(rng, 20):
            name += " " + (rng.choice(FORMAT_TAGS) if al["kind"] != "flac" else "[FLAC]")
    elif rel == "suffix":
        k = rng.random()
        if al["kind"] == "flac" and k < 0.6:
            suffix = rng.choice(("[FLAC]", "[16-44]", "[24-96]", "(FLAC)"))
        elif k < 0.55:
            suffix = rng.choice(FORMAT_TAGS)
        else:
            suffix = rng.choice(EDITIONS)
        name = f"{prefix}{base} {suffix}" if prefix or pct(rng, 70) else f"{a['folder']} - {base}"
    elif rel == "other":
        name = rng.choice((f"{base.split(' ')[0]} {rng.randint(1, 9)}", "Untitled", "Album",
                           f"{base} Sessions", "".join(w[:1] for w in base.split(" ")).upper() or "X"))
    else:  # case
        k = rng.random()
        if base.startswith("The ") and k < 0.3:
            name = base[4:] + ", The"
        elif k < 0.65:
            name = base.lower() if base != base.lower() else base.upper()
        else:
            name = base.replace(" & ", " and ").replace(",", "") if (" & " in base or "," in base) else base.title()
            if name == base:
                name = base.upper()
    return fat_safe(name), rel


def _tag_style(rng, kind):
    """The tag container and its details, per album."""
    st = {"kind": kind}
    if kind == "mp3":
        st["notags"] = pct(rng, RATES["no_tags"])
        st["v1only"] = not st["notags"] and pct(rng, RATES["id3v1_only"])
        st["id3"] = wpick(rng, RATES["id3"])
        st["v1"] = st["v1only"] or pct(rng, RATES["id3v1"])
        st["ape"] = not st["v1only"] and pct(rng, RATES["ape"])
        st["unsync"] = st["id3"] == "2.4" and pct(rng, RATES["unsync_v24"])
        if st["id3"] == "2.4":
            st["enc"] = wpick(rng, {3: 50, 0: 30, 1: 15, 2: 5})
        else:
            st["enc"] = wpick(rng, {0: 70, 1: 30})
        st["xing"] = pct(rng, RATES["xing"])
        st["lame"] = st["xing"] and pct(rng, 100 * RATES["lame"] / RATES["xing"] if RATES["xing"] else 0)
        st["info"] = st["xing"] and pct(rng, 30)
        st["tlen"] = pct(rng, RATES["tlen"])
        st["comm"] = pct(rng, RATES["comm"])
        st["pic_first"] = pct(rng, RATES["pic_first"])
        st["padding"] = rng.choice((0, 64, 256, 512, 1024))
        st["numeric_genre"] = st["id3"] in ("2.3", "2.2") and pct(rng, 12)
        st["slash_track"] = pct(rng, 40)
    else:
        st["notags"] = False
        st["padding"] = rng.choice((0, 128, 512)) if kind == "flac" else 0
        st["seektable"] = kind == "flac" and pct(rng, 70)
        st["alias"] = pct(rng, 3)  # ALBUMARTIST and "ALBUM ARTIST" both (the duplicate case)
        st["lowkeys"] = pct(rng, 5)
        st["slash_track"] = pct(rng, 15)
    return st


def _fill_album(plan, rng, names, a, al, album_seen, genre_w, year_w):
    kind = al["kind"]
    va = al.get("va", False)
    while True:
        tag_album = names.album(a["tag"]) if not va else f"{names.phrase()} Vol. {rng.randint(1, 12)}"
        if fold(tag_album) not in album_seen and len(tag_album) < 70:
            break
    album_seen.add(fold(tag_album))
    year = wpick(rng, year_w)
    folder, rel = _album_folder_name(rng, a, al, tag_album, year)
    if al["layout"] == "siblings":
        mark = rng.choice(("(Disc {d})", "CD{d}", "(CD {d})", "- Disc {d}"))
        in_tag = pct(rng, 75)
    while fold(folder) in {fold(x) for x in a.get("_folders", ())}:
        folder += " (2)"
    a.setdefault("_folders", set()).add(folder)
    al.update(tag_album=tag_album, year=year, folder=folder, rel=rel)
    st = _tag_style(rng, kind if kind in ("mp3", "flac", "opus") else "other")
    al["style"] = st
    # Album-level tags.
    aa_pct = RATES["albumartist_mp3"] if kind == "mp3" else RATES["albumartist_flac"]
    album_artist = None
    if va:
        album_artist = "Various Artists"
    elif pct(rng, aa_pct):
        album_artist = a["tag"]
    compilation = pct(rng, RATES["compilation_va"]) if va else pct(rng, RATES["compilation_other"])
    gpct = RATES["genre_flac"] if kind != "mp3" else RATES["genre_mp3"]
    genre = [wpick(rng, genre_w)] if pct(rng, gpct) else []
    if genre and kind != "mp3" and pct(rng, 40):
        g2 = wpick(rng, genre_w)
        if g2 != genre[0]:
            genre.append(g2)
    if genre and kind == "mp3" and st.get("id3") == "2.4" and pct(rng, 10):
        g2 = wpick(rng, genre_w)
        if g2 != genre[0]:
            genre.append(g2)
    # Multi-disc folders carry their disc numbers on a third (2.1 % of tracks say a disc above 1).
    has_disc = pct(rng, RATES["disc_multi"]) if al["discs"] > 1 else pct(rng, RATES["disc"])
    has_year = pct(rng, RATES["year"] + 2)
    picture = (kind == "mp3" and pct(rng, RATES["picture_mp3"])) or (kind == "flac" and pct(rng, RATES["picture_flac"])) \
        or (kind == "opus" and pct(rng, RATES["picture_opus"]))
    pic_kind = wpick(rng, {"jpeg": 88, "progressive": 6, "png": 6}) if picture else None
    rg = pct(rng, RATES["rg"])
    mbid = pct(rng, RATES["mbid"])
    album_sort = tag_album[4:] + ", The" if tag_album.startswith("The ") and pct(rng, 5) else None
    shape = al.get("names") or wpick(rng, NAME_SHAPES)
    if va and pct(rng, 70) and shape not in NUMBERLESS:
        shape = "NN - Artist - Title"
    if al["discs"] > 1 and shape in ("Title", "Artist - Title", "NN_title") and al.get("names") is None:
        shape = "NN - Title"
    digits1 = pct(rng, 5)
    ext_upper = pct(rng, 2)
    hidden_twins = pct(rng, 0.25)
    disc_names = None
    if al["layout"] in ("subfolders", "deep"):
        style = rng.choice(("CD{d}", "CD {d}", "Disc {d}", "Disc {d} - {t}"))
        disc_names = []
        for d in range(1, al["discs"] + 1):
            disc_names.append(fat_safe(style.format(d=d, t=names.phrase())))
    album_dirs = []
    base_rel = f"{a['folder_rel']}/{folder}"
    if al["layout"] == "siblings":
        for d in range(1, 3):
            fname = fat_safe(f"{folder} {mark.format(d=d)}")
            album_dirs.append(f"{a['folder_rel']}/{fname}")
            plan.folders.append(album_dirs[-1])
    elif al["layout"] in ("subfolders", "deep"):
        plan.folders.append(base_rel)
        for dn in disc_names:
            album_dirs.append(f"{base_rel}/{dn}")
            plan.folders.append(album_dirs[-1])
        if al["layout"] == "deep":
            album_dirs[0] = album_dirs[0] + "/Bonus"
            plan.folders.append(album_dirs[0])
    else:
        plan.folders.append(base_rel)
        album_dirs.append(base_rel)
    al["dirs"] = album_dirs
    al["rel_dir"] = base_rel
    # The tracks.
    per = al.get("per_disc") or [al["n"]]
    va_pool = [x for x in plan.artists if not x["va"]]
    seen_names = {}
    tracks = []
    tnum = 0
    for d, n in enumerate(per, start=1):
        for k in range(1, n + 1):
            tnum += 1
            while True:
                t = names.title()
                if fold(t) not in seen_names:
                    break
            seen_names[fold(t)] = 1
            track_artist = [a["tag"]]
            if va:
                other_a = rng.choice(va_pool)["tag"] if va_pool and pct(rng, 60) else names.artist()
                track_artist = [other_a]
            elif pct(rng, RATES["multi_artist"]):
                track_artist = [a["tag"], names.artist()]
            if pct(rng, RATES["feat_title"]) and not t.startswith(tuple("0123456789")):
                t = f"{t} (feat. {names.word()} {names.word()})"
            dir_rel = album_dirs[d - 1] if al["layout"] in ("siblings", "subfolders") else album_dirs[0]
            if al["layout"] == "deep":
                dir_rel = album_dirs[0]
            number = k if (al["discs"] > 1 and al["layout"] != "flat") or al.get("names") else tnum
            if al.get("names") == "restart":
                number = k
            tracks.append({"title": t, "artist": track_artist, "disc": d, "number": number, "track_in_disc": k,
                           "dir": dir_rel})
    disc_total = len(per) if has_disc else 0
    album_tag = tag_album
    if al["layout"] == "siblings" and in_tag:
        album_tag_by_disc = {d: f"{tag_album} {mark.format(d=d)}".replace("  ", " ") for d in (1, 2)}
    else:
        album_tag_by_disc = None
    for i, tr in enumerate(tracks):
        tags = {}
        title_rel = wpick(rng, {k: v for k, v in RATES["title"].items() if k != "artist"})
        if shape in ARTIST_SHAPES:
            title_rel = "artist"
        file_title = tr["title"]
        tag_title = tr["title"]
        if title_rel == "inside":
            file_title = f"{tr['title']} {rng.choice(SUFFIXES)}"
        elif title_rel == "case":
            file_title = tr["title"].lower() if tr["title"] != tr["title"].lower() else tr["title"].upper()
            if pct(rng, 40):
                file_title = tr["title"].replace("\u2019", "'").replace(",", "")
                if file_title == tr["title"]:
                    file_title = tr["title"].lower()
        elif title_rel == "longer":
            tag_title = f"{tr['title']} {rng.choice(LONGER)}"
        elif title_rel == "other":
            file_title = rng.choice((f"Track {tr['number']:02d}", names.phrase(), f"{names.phrase()} (Edit)"))
        elif title_rel == "none":
            tag_title = None
        tags["title"] = tag_title
        tags["artist"] = tr["artist"]
        tags["album"] = (album_tag_by_disc[tr["disc"]] if album_tag_by_disc else album_tag) \
            if not pct(rng, RATES["album_none"]) else None
        tags["albumartist"] = [album_artist] if album_artist else []
        if pct(rng, RATES["track"]):
            tags["track"] = tr["number"]
            tags["tracktotal"] = per[tr["disc"] - 1] if st.get("slash_track") else 0
        if has_disc:
            tags["disc"] = tr["disc"]
            tags["disctotal"] = disc_total if pct(rng, 60) else 0
        if has_year and pct(rng, 99):
            tags["year"] = year
            tags["date"] = f"{year}" if pct(rng, 80) else f"{year}-{rng.randint(1, 12):02d}-{rng.randint(1, 28):02d}"
        tags["genre"] = list(genre)
        if compilation:
            tags["compilation"] = 1
        if a.get("sort") and tags["artist"] and tags["artist"][0] == a["tag"]:
            tags["artistsort"] = a["sort"]
            if album_artist == a["tag"]:
                tags["albumartistsort"] = a["sort"]
        if album_sort:
            tags["albumsort"] = album_sort
        if mbid:
            tags["mbalbum"] = _uuid(rng)
            tags["mbtrack"] = _uuid(rng)
        if rg:
            tags["rg_track"] = f"{rng.uniform(-11, 2):.2f} dB"
            tags["rg_album"] = f"{rng.uniform(-10, 1):.2f} dB"
        if pct(rng, RATES["bpm"]):
            tags["bpm"] = rng.randint(70, 175)
        if st["notags"]:
            tags = {}
        # The file's name.
        fname = _file_name(rng, shape, file_title, tr, a, al, tag_album, digits1)
        ext = {"mp3": ".mp3", "flac": ".flac", "opus": ".opus"}.get(kind, "." + kind)
        if ext_upper and kind == "mp3":
            ext = ".MP3"
        stem = fname
        while fold(stem + ext) in {fold(x) for x in al.setdefault("_names", set())}:
            stem += " (2)"
        al["_names"].add(stem + ext)
        rel = f"{tr['dir']}/{stem}{ext}"
        if len(rel.encode("utf-8")) > 240:
            rel = f"{tr['dir']}/{tr['number']:02d} {stem[:40].rstrip(' .')}{ext}"
        f = {"rel": rel, "kind": kind, "tags": tags, "style": st, "shape": shape, "title_rel": title_rel,
             "artist_rel": a["rel"], "album_rel": rel_album(al), "va": va, "pic": pic_kind,
             "frames": rng.randint(24, 39) if kind == "mp3" else 0,
             "seconds": rng.randint(2, 5) if kind in ("flac", "opus") else 0,
             "tlen": st.get("tlen", False), "mtime": _mtime(rng, year), "album_id": id(al)}
        plan.files.append(f)
        if hidden_twins:
            plan.files.append({"rel": f"{tr['dir']}/._{stem}{ext}", "kind": "hidden", "mtime": f["mtime"]})
    _album_images(plan, rng, al, kind)


def rel_album(al):
    return al.get("rel", "same")


def _uuid(rng):
    h = "".join(rng.choice("0123456789abcdef") for _ in range(32))
    return f"{h[:8]}-{h[8:12]}-{h[12:16]}-{h[16:20]}-{h[20:]}"


def _mtime(rng, year):
    """A FAT-representable time (2 s steps), after the album's year (and not before 2004)."""
    y = max(2004, min(2025, year + rng.randint(0, 6)))
    t = time.mktime((y, rng.randint(1, 12), rng.randint(1, 28), rng.randint(0, 23), rng.randint(0, 59),
                     rng.randrange(0, 60, 2), 0, 0, -1))
    return int(t) // 2 * 2


def _file_name(rng, shape, title, tr, a, al, tag_album, digits1):
    n = tr["number"]
    nn = f"{n}" if digits1 and n < 10 else f"{n:02d}"
    t = title
    artist = a["folder"] if not al.get("va") else fat_safe(tr["artist"][0])
    if shape == "D-NN Title":
        name = rng.choice((f"{tr['disc']}-{tr['track_in_disc']:02d} {t}", f"{tr['disc']}-{tr['track_in_disc']:02d} - {t}"))
    elif shape == "DNN - Title":
        name = f"{tr['disc']}{tr['track_in_disc']:02d} - {t}"
    elif shape == "restart":
        name = f"{tr['track_in_disc']:02d} - {t}"
    elif shape == "NN - Title":
        name = f"{nn} - {t}"
    elif shape == "NN Title":
        name = f"{nn} {t}"
    elif shape == "NN. Title":
        name = f"{nn}. {t}"
    elif shape == "NN-Title":
        name = f"{nn}-{t}"
    elif shape == "NN_title":
        name = f"{nn}_{t.lower().replace(' ', '_')}"
    elif shape == "NN - Artist - Title":
        name = f"{nn} - {artist} - {t}"
    elif shape == "NN Artist - Title":
        name = f"{nn} {artist} - {t}"
    elif shape == "Artist - NN - Title":
        name = f"{artist} - {nn} - {t}"
    elif shape == "Artist - Album - NN - Title":
        name = f"{artist} - {fat_safe(tag_album)} - {nn} - {t}"
    elif shape == "(NN) Title":
        name = f"({nn}) {t}"
    elif shape == "Artist - Title":
        name = f"{artist} - {t}"
    else:
        name = t
    return fat_safe(name)


def _album_images(plan, rng, al, kind):
    """Covers and the other files: named covers in 36 % of audio folders,
    another .jpg only in 13 %, a .png only in 1.6 %; .cue, .log, .m3u ...;
    a Scans folder now and then."""
    for d in sorted(set(al["dirs"])):
        if al.get("probe_safe"):
            continue

        def jpg(name, progressive=None):
            plan.files.append({"rel": f"{d}/{name}", "kind": "jpg", "mtime": _mtime(rng, 2010),
                               "progressive": pct(rng, 15) if progressive is None else progressive})
        k = rng.random() * 100
        if k < 36:
            jpg(wpick(rng, {"cover.jpg": 55, "folder.jpg": 25, "Cover.jpg": 8, "front.jpg": 7, "Folder.jpg": 5}))
            if pct(rng, 30):
                jpg("AlbumArtSmall.jpg", False)
            if pct(rng, 22):
                jpg(rng.choice(("back.jpg", "Back.jpg", "cd.jpg", "inlay.jpg", "inside.jpg")))
        elif k < 49:
            for name in rng.sample(("art.jpg", "scan.jpeg", "00.jpg", "back.jpg", "booklet.jpg", "disc.jpg",
                                    f"{fat_safe(al['tag_album'])[:40]}.jpg"), rng.randint(1, 3)):
                jpg(name)
        elif k < 50.6:
            plan.files.append({"rel": f"{d}/cover.png", "kind": "png", "mtime": _mtime(rng, 2010)})
        r = rng.random() * 100
        for ext, p in ((".cue", 11), (".log", 11), (".m3u", 27), (".nfo", 6), (".txt", 6), (".sfv", 5), (".md5", 4)):
            if rng.random() * 100 < p:
                stem = fat_safe(al["tag_album"])[:40] if ext in (".cue", ".log", ".m3u") else "info"
                plan.files.append({"rel": f"{d}/{stem}{ext}", "kind": "text", "ext": ext, "album": al["tag_album"],
                                   "mtime": _mtime(rng, 2010)})
        if r < 7:
            plan.files.append({"rel": f"{d}/Thumbs.db", "kind": "blob", "mtime": _mtime(rng, 2010)})
        elif r < 11:
            plan.files.append({"rel": f"{d}/desktop.ini", "kind": "text", "ext": ".ini", "album": "", "mtime": _mtime(rng, 2010)})
        if r > 99.2:
            plan.files.append({"rel": f"{d}/.DS_Store", "kind": "hidden", "mtime": _mtime(rng, 2010)})
    if not al.get("probe_safe") and al["layout"] != "siblings" and pct(rng, 3):
        scans = f"{al['rel_dir']}/{rng.choice(('Scans', 'Artwork', 'Covers'))}"
        plan.folders.append(scans)
        for i in range(rng.randint(3, 8)):
            plan.files.append({"rel": f"{scans}/{i + 1:02d}.jpg", "kind": "jpg", "progressive": pct(rng, 15),
                               "mtime": _mtime(rng, 2010)})


def _fill_loose(plan, rng, names, a, genre_w, year_w):
    seen = set()
    for _ in a["loose"]:
        while True:
            t = names.title()
            if fold(t) not in seen:
                break
        seen.add(fold(t))
        st = _tag_style(rng, "mp3")
        year = wpick(rng, year_w)
        tags = {"title": t, "artist": [a["tag"]], "album": None if pct(rng, 50) else names.phrase(),
                "albumartist": [], "genre": [wpick(rng, genre_w)] if pct(rng, 70) else []}
        if pct(rng, 90):
            tags["year"] = year
            tags["date"] = str(year)
        name = fat_safe(rng.choice((t, f"{a['folder']} - {t}")))
        plan.files.append({"rel": f"{a['folder_rel']}/{name}.mp3", "kind": "mp3", "tags": tags, "style": st,
                           "shape": "loose", "title_rel": "same" if name == fat_safe(t) else "artist",
                           "artist_rel": a["rel"], "album_rel": "loose", "va": False, "pic": None,
                           "frames": rng.randint(24, 39), "seconds": 0, "tlen": False, "mtime": _mtime(rng, year),
                           "album_id": None})


def _probes(plan, rng):
    """L0's three probe albums: the artists at entries 1, 353 and 705 of
    /music in creation order (NTFS's name order; the nearest one with a
    plain MP3 album when that artist has none), each album first among the
    artist's candidates: no pictures, no covers, no other files, at depth 2."""
    order = sorted((a for a in plan.artists), key=lambda a: ntfs_key(a["folder"]))
    plan.probes = []
    n = len(order)

    def candidates(a):
        if a["va"] or not a["folder"].isascii():
            return []
        return [al for al in a["albums"] if al["layout"] == "flat" and al["kind"] == "mp3" and al["n"] >= 2
                and not al.get("va")]
    taken = set()
    for want in PROBE_POSITIONS:
        if want > n:
            continue
        pos = None
        for off in range(0, 40):
            for p in (want - off, want + off):
                if 1 <= p <= n and p not in taken and candidates(order[p - 1]):
                    pos = p
                    break
            if pos:
                break
        if pos is None:
            continue
        taken.add(pos)
        a = order[pos - 1]
        al = sorted(candidates(a), key=lambda x: ntfs_key(x["folder"]))[0]
        al["probe_safe"] = True
        files = [f for f in plan.files if f.get("album_id") == id(al)]
        # No pictures, no covers, no hidden twins: an open is its path's lookup and the first frames.
        for f in files:
            f["pic"] = None
        plan.files = [f for f in plan.files if not (f["rel"].startswith(al["rel_dir"] + "/")
                                                    and f["kind"] in ("jpg", "png", "hidden", "text", "blob"))]
        plan.folders = [d for d in plan.folders if not d.startswith(al["rel_dir"] + "/")]
        first = sorted((f for f in files if f["kind"] == "mp3"), key=lambda f: ntfs_key(f["rel"]))[0]
        plan.probes.append({"position": pos, "wanted": want, "artist": a["folder"], "album": al["folder"],
                            "first": first["rel"], "tracks": al["n"]})


def _extras(plan, rng, names, s):
    """The folders a library holds besides its albums (image-only, empty),
    up to the measured 2,591 folders."""
    target = scaled(MEASURED["folders"], s, 1)
    normal = [a for a in plan.artists if not a["va"]]
    i = 0
    while len(plan.folders) < target and normal:
        a = normal[rng.randrange(len(normal))]
        name = rng.choice(("Artwork", "Misc", "Singles", "old", "Photos", "Extras", "Videos", "Bootlegs"))
        rel = f"{a['folder_rel']}/{name}"
        if rel in plan.folders:
            rel = f"{rel} {i}"
        plan.folders.append(rel)
        if pct(rng, 40):
            plan.files.append({"rel": f"{rel}/{rng.randint(1, 99):02d}.jpg", "kind": "jpg", "progressive": False,
                               "mtime": _mtime(rng, 2012)})
        i += 1


# ---------------------------------------------------------------------------
# Bytes
# ---------------------------------------------------------------------------


def _enc_ok(enc, s):
    if enc != 0:
        return True
    try:
        s.encode("latin-1")
        return True
    except UnicodeEncodeError:
        return False


def _enc_text(enc, s):
    if enc == 0:
        return s.encode("latin-1")
    if enc == 1:
        return b"\xff\xfe" + s.encode("utf-16-le")
    if enc == 2:
        return s.encode("utf-16-be")
    return s.encode("utf-8")


def _term(enc):
    return b"\0\0" if enc in (1, 2) else b"\0"


class Id3:
    """An ID3v2.2/2.3/2.4 tag, frame by frame."""
    V22 = {"TIT2": "TT2", "TPE1": "TP1", "TALB": "TAL", "TPE2": "TP2", "TRCK": "TRK", "TPOS": "TPA", "TYER": "TYE",
           "TCON": "TCO", "TCMP": "TCP", "TBPM": "TBP", "TLEN": "TLE", "TXXX": "TXX", "COMM": "COM", "APIC": "PIC",
           "TSOP": "TSP", "TSO2": "TS2", "TSOA": "TSA"}

    def __init__(self, ver, enc):
        self.ver, self.enc = ver, enc
        self.frames = []  # (id, body, picture prefix length or None)

    def _fenc(self, *values):
        enc = self.enc
        if any(not _enc_ok(enc, v) for v in values):
            enc = 3 if self.ver == 4 else 1
        if self.ver != 4 and enc in (2, 3):
            enc = 1
        return enc

    def text(self, fid, values):
        values = [v for v in values if v]
        if not values:
            return
        if self.ver != 4 and len(values) > 1:
            values = [" / ".join(values)]  # v2.2/2.3: one string
        enc = self._fenc(*values)
        body = bytes([enc]) + _term(enc).join(_enc_text(enc, v) for v in values)
        self.frames.append((fid, body, None))

    def txxx(self, desc, value):
        enc = self._fenc(desc, value)
        self.frames.append(("TXXX", bytes([enc]) + _enc_text(enc, desc) + _term(enc) + _enc_text(enc, value), None))

    def comm(self, text):
        enc = self._fenc(text)
        self.frames.append(("COMM", bytes([enc]) + b"eng" + _term(enc) + _enc_text(enc, text), None))

    def picture(self, data, mime, first=False):
        if self.ver == 2:
            pre = b"\0" + (b"PNG" if mime == "image/png" else b"JPG") + b"\x03" + b"\0"
        else:
            pre = b"\0" + mime.encode() + b"\0\x03" + b"\0"
        fr = ("APIC", pre + data, len(pre))
        if first:
            self.frames.insert(0, fr)
        else:
            self.frames.append(fr)

    def build(self, padding, unsync_all=False):
        """(bytes, the picture's (offset, stored length, coding) in the tag or None)."""
        out = bytearray()
        pic = None
        for fid, body, pic_pre in self.frames:
            start = len(out)
            if self.ver == 2:
                fid2 = self.V22.get(fid, fid[:3])
                out += fid2.encode() + struct.pack(">I", len(body))[1:]
                data_at = len(out)
                out += body
                stored_pre, stored = pic_pre, body
            elif self.ver == 3:
                out += fid.encode() + struct.pack(">I", len(body)) + b"\0\0"
                data_at = len(out)
                out += body
                stored_pre, stored = pic_pre, body
            else:
                if unsync_all:
                    u = unsync(body)
                    stored = syncsafe(len(body)) + u
                    out += fid.encode() + syncsafe(len(stored)) + b"\x00\x03"
                    data_at = len(out) + 4
                    stored_pre = pic_pre
                    out += stored
                    stored = u
                else:
                    out += fid.encode() + syncsafe(len(body)) + b"\0\0"
                    data_at = len(out)
                    out += body
                    stored_pre, stored = pic_pre, body
            if pic_pre is not None and pic is None:
                pic = (10 + data_at + stored_pre, len(stored) - stored_pre, 1 if (self.ver == 4 and unsync_all) else 0)
            del start
        out += bytes(padding)
        flags = 0x80 if (self.ver == 4 and unsync_all) else 0
        head = b"ID3" + bytes([self.ver, 0, flags]) + syncsafe(len(out))
        return head + bytes(out), pic


def _latin1(s, n):
    b = (s or "").encode("latin-1", "replace")[:n]
    return b + bytes(n - len(b))


def id3v1(tags, genre_index):
    year = str(tags.get("year", ""))[:4].encode() if tags.get("year") else b""
    track = tags.get("track", 0) or 0
    return (b"TAG" + _latin1(tags.get("title"), 30) + _latin1(", ".join(tags.get("artist") or []), 30)
            + _latin1(tags.get("album"), 30) + year + bytes(4 - len(year)) + bytes(28) + b"\0"
            + bytes([min(255, track)]) + bytes([genre_index]))


def ape_tag(tags):
    items = []
    for key, val in (("Title", tags.get("title")), ("Artist", "; ".join(tags.get("artist") or [])),
                     ("Album", tags.get("album")), ("Track", str(tags["track"]) if tags.get("track") else None),
                     ("Year", str(tags["year"]) if tags.get("year") else None)):
        if val:
            v = val.encode("utf-8")
            items.append(struct.pack("<II", len(v), 0) + key.encode() + b"\0" + v)
    body = b"".join(items)
    size = len(body) + 32

    def hf(is_header):
        flags = (1 << 31) | ((1 << 29) if is_header else 0)
        return b"APETAGEX" + struct.pack("<IIII", 2000, size, len(items), flags) + bytes(8)
    return hf(True) + body + hf(False)


def mp3_audio(frames, xing, info, lame):
    """Silent MPEG-1 Layer III, 44.1 kHz mono: an optional Xing/Info frame
    (128 kbps, with a TOC; LAME's extension), then `frames` frames at
    32 kbps (104/105 bytes). (bytes, length in ms as the header gives it)."""
    out = bytearray()
    audio = bytearray()
    acc = 0
    for _ in range(frames):
        acc += 21600  # 144 * 32000 % 44100
        pad = 0
        if acc >= 44100:
            acc -= 44100
            pad = 1
        hdr = bytes([0xFF, 0xFB, 0x10 | (pad << 1), 0xC0])
        audio += hdr + bytes(104 + pad - 4)
    delay, padding = 576, 1152
    if xing:
        f = bytearray(417)
        f[0:4] = bytes([0xFF, 0xFB, 0x90, 0xC0])
        x = 4 + 17
        f[x:x + 4] = b"Info" if info else b"Xing"
        f[x + 4:x + 8] = struct.pack(">I", 0x0F)
        f[x + 8:x + 12] = struct.pack(">I", frames)
        f[x + 12:x + 16] = struct.pack(">I", len(audio) + 417)
        for i in range(100):
            f[x + 16 + i] = i * 256 // 100
        f[x + 116:x + 120] = struct.pack(">I", 57)
        if lame:
            e = x + 120
            f[e:e + 9] = b"LAME3.100"
            f[e + 21] = delay >> 4
            f[e + 22] = ((delay & 0xF) << 4) | (padding >> 8)
            f[e + 23] = padding & 0xFF
            c = crc16_lame(bytes(f[:e + 34]))
            f[e + 34] = c >> 8
            f[e + 35] = c & 0xFF
        out += f
        kept = frames * 1152 - (delay + padding if lame else 0)
    else:
        kept = frames * 1152
    out += audio
    return bytes(out), kept * 1000 // 44100  # (as progress::mp3HeaderDurationMs(): truncated)


def _genre_values(tags, numeric, id3ver):
    vals = list(tags.get("genre") or [])
    if numeric and vals and vals[0] in ID3V1_GENRES:
        vals[0] = f"({ID3V1_GENRES.index(vals[0])})"
    return vals


def build_mp3(f):
    st, tags = f["style"], f["tags"]
    audio, ms = mp3_audio(f["frames"], st["xing"], st.get("info"), st["lame"])
    head = b""
    pic_anchor = None
    written = {}
    if not st["notags"] and not st["v1only"]:
        ver = {"2.2": 2, "2.3": 3, "2.4": 4}[st["id3"]]
        t = Id3(ver, st["enc"])
        t.text("TIT2", [tags.get("title")] if tags.get("title") else [])
        t.text("TPE1", tags.get("artist") or [])
        t.text("TALB", [tags.get("album")] if tags.get("album") else [])
        t.text("TPE2", tags.get("albumartist") or [])
        if tags.get("track"):
            t.text("TRCK", [f"{tags['track']}/{tags['tracktotal']}" if tags.get("tracktotal") else str(tags["track"])])
        if tags.get("disc"):
            t.text("TPOS", [f"{tags['disc']}/{tags['disctotal']}" if tags.get("disctotal") else str(tags["disc"])])
        if tags.get("year"):
            if ver == 4:
                t.text("TDRC", [tags.get("date") or str(tags["year"])])
            else:
                t.text("TYER", [str(tags["year"])])
        t.text("TCON", _genre_values(tags, st.get("numeric_genre"), ver))
        if tags.get("compilation"):
            t.text("TCMP", ["1"])
        if tags.get("artistsort"):
            t.text("TSOP", [tags["artistsort"]])
        if tags.get("albumartistsort"):
            t.text("TSO2", [tags["albumartistsort"]])
        if tags.get("albumsort"):
            t.text("TSOA", [tags["albumsort"]])
        if tags.get("bpm"):
            t.text("TBPM", [str(tags["bpm"])])
        if st.get("tlen"):
            t.text("TLEN", [str(ms)])
        if tags.get("mbalbum") and ver != 2:
            t.txxx("MusicBrainz Album Id", tags["mbalbum"])
        if tags.get("rg_track") and ver != 2:
            t.txxx("REPLAYGAIN_TRACK_GAIN", tags["rg_track"])
            t.txxx("REPLAYGAIN_ALBUM_GAIN", tags["rg_album"])
        if st.get("comm"):
            t.comm("synthetic")
        if f.get("pic"):
            data, mime = _picture(f["pic"])
            t.picture(data, mime, first=st.get("pic_first"))
        head, pic = t.build(st["padding"], unsync_all=st.get("unsync"))
        if pic:
            pic_anchor = {"offset": pic[0], "length": pic[1], "coding": pic[2], "mime": f["pic"]}
        written["id3"] = st["id3"]
    tail = b""
    if not st["notags"] and st.get("ape") and not st["v1only"]:
        tail += ape_tag(tags)
        written["ape"] = True
    if not st["notags"] and st["v1"]:
        g = tags.get("genre") or []
        gi = ID3V1_GENRES.index(g[0]) if g and g[0] in ID3V1_GENRES else 255
        tail += id3v1(tags, gi)
        written["v1"] = True
    return head + audio + tail, ms, pic_anchor, written


def _picture(kind):
    if kind == "png":
        return PNG_SMALL, "image/png"
    if kind == "progressive":
        return JPEG_PROGRESSIVE, "image/jpeg"
    return JPEG_BASELINE, "image/jpeg"


def flac_picture_block(data, mime):
    m = mime.encode()
    return (struct.pack(">II", 3, len(m)) + m + struct.pack(">I", 0) + struct.pack(">IIII", 48, 48, 24, 0)
            + struct.pack(">I", len(data)) + data), 32 + len(m)


def vorbis_items(tags, st, kind):
    it = []

    def add(k, v):
        if v not in (None, "", 0):
            it.append((k.lower() if st.get("lowkeys") else k, str(v)))
    add("TITLE", tags.get("title"))
    for a in tags.get("artist") or []:
        add("ARTIST", a)
    add("ALBUM", tags.get("album"))
    for a in tags.get("albumartist") or []:
        add("ALBUMARTIST", a)
        if st.get("alias"):
            add("ALBUM ARTIST", a)
    if tags.get("track"):
        if st.get("slash_track") and tags.get("tracktotal"):
            add("TRACKNUMBER", f"{tags['track']}/{tags['tracktotal']}")
        else:
            add("TRACKNUMBER", tags["track"])
            add("TRACKTOTAL", tags.get("tracktotal"))
    add("DISCNUMBER", tags.get("disc"))
    add("DISCTOTAL", tags.get("disctotal"))
    add("DATE", tags.get("date"))
    for g in tags.get("genre") or []:
        add("GENRE", g)
    add("COMPILATION", tags.get("compilation"))
    add("ARTISTSORT", tags.get("artistsort"))
    add("ALBUMARTISTSORT", tags.get("albumartistsort"))
    add("ALBUMSORT", tags.get("albumsort"))
    add("MUSICBRAINZ_ALBUMID", tags.get("mbalbum"))
    add("MUSICBRAINZ_TRACKID", tags.get("mbtrack"))
    if tags.get("rg_track"):
        if kind == "opus":
            add("R128_TRACK_GAIN", int(round((float(tags["rg_track"].split()[0]) - 5) * 256)))
        else:
            add("REPLAYGAIN_TRACK_GAIN", tags["rg_track"])
            add("REPLAYGAIN_ALBUM_GAIN", tags["rg_album"])
    add("BPM", tags.get("bpm"))
    return it


def vorbis_comment(items, vendor):
    v = vendor.encode()
    out = struct.pack("<I", len(v)) + v + struct.pack("<I", len(items))
    for k, val in items:
        b = f"{k}={val}".encode("utf-8")
        out += struct.pack("<I", len(b)) + b
    return out


def _utf8num(v):
    if v < 0x80:
        return bytes([v])
    if v < 0x800:
        return bytes([0xC0 | (v >> 6), 0x80 | (v & 0x3F)])
    return bytes([0xE0 | (v >> 12), 0x80 | ((v >> 6) & 0x3F), 0x80 | (v & 0x3F)])


def flac_frames(total, block=4096):
    out = bytearray()
    sizes = []
    n = 0
    fn = 0
    while n < total:
        bs = min(block, total - n)
        h = bytearray(b"\xFF\xF8")
        code = 0xC if bs == 4096 else 0x7
        h.append((code << 4) | 0x9)   # 44.1 kHz
        h.append(0x18)                # 2 channels, independent; 16 bits
        h += _utf8num(fn)
        if code == 0x7:
            h += struct.pack(">H", bs - 1)
        h.append(crc8(h))
        fr = bytes(h) + bytes(6)      # two CONSTANT subframes of 0 (a header byte and 16 bits each)
        fr += struct.pack(">H", crc16(fr))
        out += fr
        sizes.append(len(fr))
        n += bs
        fn += 1
    return bytes(out), min(sizes), max(sizes)


def build_flac(f):
    st, tags = f["style"], f["tags"]
    total = f["seconds"] * 44100
    frames, fmin, fmax = flac_frames(total)
    si = struct.pack(">HH", 4096, 4096) + struct.pack(">I", fmin)[1:] + struct.pack(">I", fmax)[1:]
    si += struct.pack(">Q", (44100 << 44) | (1 << 41) | (15 << 36) | total) + bytes(16)
    blocks = [(0, si)]
    if st.get("seektable"):
        blocks.append((3, struct.pack(">QQH", 0, 0, 4096)))
    blocks.append((4, vorbis_comment(vorbis_items(tags, st, "flac"), "reference libFLAC 1.3.2 20170101")))
    pic_anchor = None
    pic_block = None
    if f.get("pic"):
        data, mime = _picture(f["pic"])
        pic_block = flac_picture_block(data, mime)
        blocks.append((6, pic_block[0]))
    if st.get("padding"):
        blocks.append((1, bytes(st["padding"])))
    out = bytearray(b"fLaC")
    for i, (typ, body) in enumerate(blocks):
        last = i == len(blocks) - 1
        if typ == 6:
            pic_anchor = {"offset": len(out) + 4 + pic_block[1], "length": len(body) - pic_block[1], "coding": 0,
                          "mime": f["pic"]}
        out += bytes([(0x80 if last else 0) | typ]) + struct.pack(">I", len(body))[1:] + body
    out += frames
    return bytes(out), f["seconds"] * 1000, pic_anchor, {}


class Ogg:
    def __init__(self, serial):
        self.serial, self.seq, self.out = serial, 0, bytearray()

    def page(self, segs, body, granule, flags=0):
        h = bytearray(b"OggS" + bytes([0, flags]) + struct.pack("<qII", granule, self.serial, self.seq) + bytes(4))
        h += bytes([len(segs)]) + bytes(segs)
        p = bytearray(h + body)
        p[22:26] = struct.pack("<I", ogg_crc(bytes(p)))
        self.seq += 1
        at = len(self.out) + len(h)
        self.out += p
        return at

    def packet(self, data, granule, flags=0):
        """One packet, over as many pages as it takes. [(file offset, packet offset, length)]."""
        pieces, pos, cont = [], 0, False
        n = len(data)
        while True:
            segs, take = [], 0
            while len(segs) < 255:
                left = n - pos - take
                if left >= 255:
                    segs.append(255)
                    take += 255
                else:
                    segs.append(left)
                    take += left
                    break
            done = segs[-1] < 255
            at = self.page(segs, data[pos:pos + take], granule if done else -1, (1 if cont else 0) | (flags if not cont else 0))
            pieces.append((at, pos, take))
            pos += take
            cont = True
            if done:
                return pieces


SILENCE_STEREO = b"\xFC\xFF\xFE"  # an Opus packet: CELT fullband 20 ms, stereo, the silence flag


def build_opus(f):
    st, tags = f["style"], f["tags"]
    serial = zlib.crc32(f["rel"].encode("utf-8")) & 0xFFFFFFFF
    o = Ogg(serial)
    preskip = 312
    o.page([19], b"OpusHead" + bytes([1, 2]) + struct.pack("<HIhB", preskip, 44100, 0, 0), 0, flags=2)
    items = vorbis_items(tags, st, "opus")
    pic_anchor = None
    pic_key_at = None
    if f.get("pic"):
        data, mime = _picture(f["pic"])
        block = flac_picture_block(data, mime)[0]
        items.append(("METADATA_BLOCK_PICTURE", base64.b64encode(block).decode()))
        pic_key_at = len(items) - 1
    tags_pkt = bytearray(b"OpusTags")
    v = b"synthcard (libopus 1.4)"
    tags_pkt += struct.pack("<I", len(v)) + v + struct.pack("<I", len(items))
    value_at = None
    for i, (k, val) in enumerate(items):
        b = f"{k}={val}".encode("utf-8")
        tags_pkt += struct.pack("<I", len(b))
        if i == pic_key_at:
            value_at = len(tags_pkt) + len(k) + 1
            value_len = len(b) - len(k) - 1
        tags_pkt += b
    pieces = o.packet(bytes(tags_pkt), 0)
    if value_at is not None:
        for at, start, take in pieces:
            if start <= value_at < start + take:
                pic_anchor = {"offset": at + value_at - start, "length": value_len, "coding": 2, "mime": f["pic"]}
                break
    # A page's granule counts the samples decoded to its end, the pre-skip's
    # included (RFC 7845 4); one packet more than the length, and the last
    # page's granule trims it: exactly `seconds` once the pre-skip is dropped.
    packets = f["seconds"] * 50 + 1
    done = 0
    while done < packets:
        k = min(50, packets - done)
        done += k
        last = done == packets
        o.page([3] * k, SILENCE_STEREO * k, f["seconds"] * 48000 + preskip if last else done * 960,
               flags=4 if last else 0)
    return bytes(o.out), f["seconds"] * 1000, pic_anchor, {}


def build_other(f):
    """The audio the firmware doesn't play: their containers' first bytes."""
    k = f["kind"]
    seed = zlib.crc32(f["rel"].encode("utf-8"))
    filler = bytes((seed + i * 131) & 0xFF for i in range(256))
    if k == "wma":
        return bytes.fromhex("3026B2758E66CF11A6D900AA0062CE6C") + struct.pack("<QIBB", 30 + 256, 1, 1, 2) + filler
    if k in ("m4a", "mp4"):
        brand = b"M4A " if k == "m4a" else b"isom"
        return struct.pack(">I", 24) + b"ftyp" + brand + struct.pack(">I", 0) + brand + b"mp42" + \
            struct.pack(">I", 8 + 256) + b"free" + filler
    if k == "ogg":
        o = Ogg(seed & 0xFFFFFFFF)
        o.page([30], b"\x01vorbis" + struct.pack("<IBIiiiBB", 0, 2, 44100, 0, 128000, 0, 0xB8, 1), 0, flags=2)
        return bytes(o.out)
    return filler


def file_bytes(f):
    """(bytes, length ms or None, picture anchor or None, what was written)."""
    k = f["kind"]
    if k == "mp3":
        return build_mp3(f)
    if k == "flac":
        return build_flac(f)
    if k == "opus":
        return build_opus(f)
    if k in ("wma", "m4a", "mp4", "ogg"):
        return build_other(f), None, None, {}
    if k == "jpg":
        return (JPEG_PROGRESSIVE if f.get("progressive") else JPEG_BASELINE), None, None, {}
    if k == "png":
        return PNG_SMALL, None, None, {}
    if k == "text":
        e = f.get("ext")
        if e == ".m3u":
            body = "#EXTM3U\n# synthetic playlist\n"
        elif e == ".cue":
            body = f'REM GENRE "Synthetic"\nTITLE "{f.get("album", "")}"\nFILE "image.wav" WAVE\n'
        elif e == ".log":
            body = "Synthetic rip log\nRange status and errors\nNo errors occurred\n"
        elif e == ".ini":
            body = "[.ShellClassInfo]\nIconResource=C:\\Windows\\system32\\imageres.dll,-108\n"
        else:
            body = f"synthetic {e[1:]} file\n"
        return body.encode("utf-8"), None, None, {}
    if k == "blob":
        return bytes(range(256)) * 2, None, None, {}
    if k == "hidden":
        return b"\x00\x05\x16\x07" + bytes(78), None, None, {}
    raise ValueError(k)


# ---------------------------------------------------------------------------
# Statistics
# ---------------------------------------------------------------------------

AUDIO = ("mp3", "flac", "opus")
OTHER_AUDIO = ("wma", "m4a", "mp4", "ogg")


def lfn_entries(name):
    """FAT directory entries for a name: the SFN entry, plus the LFN ones
    unless the name is a plain 8.3 name in one case (base and extension)."""
    base, dot, ext = name.rpartition(".") if "." in name else (name, "", "")
    ok = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789$%'-_@~`!(){}^#&")
    if dot and not base:
        base, ext = name, ""
    plain = (1 <= len(base) <= 8 and len(ext) <= 3 and name.count(".") <= 1
             and all(c.upper() in ok for c in base + ext)
             and (base.isupper() or base.islower() or not any(c.isalpha() for c in base))
             and (ext.isupper() or ext.islower() or not any(c.isalpha() for c in ext)))
    if plain:
        return 1
    units = len(name.encode("utf-16-le")) // 2
    return 1 + (units + 12) // 13


def percentile(xs, p):
    if not xs:
        return 0
    xs = sorted(xs)
    k = min(len(xs) - 1, max(0, int(math.ceil(p / 100.0 * len(xs))) - 1))
    return xs[k]


def summary(plan, sizes=None):
    """Counts by shape, the rates, the FAT layout and (with sizes: rel ->
    bytes) the size on a card."""
    files = plan.files
    audio = [f for f in files if f["kind"] in AUDIO]
    other_audio = [f for f in files if f["kind"] in OTHER_AUDIO]
    s = {"seed": plan.seed, "count": plan.count, "opus_share": plan.opus_share}
    s["audio_files"] = len(audio) + len(other_audio)
    s["by_kind"] = {k: sum(1 for f in files if f["kind"] == k) for k in AUDIO + OTHER_AUDIO + ("jpg", "png", "text", "blob", "hidden")}
    s["artists"] = len(plan.artists)
    s["folders"] = len(plan.folders)
    # Depth: folders between /music and the file.
    depth = {}
    per_folder = {}
    for f in audio + other_audio:
        d = f["rel"].count("/") - 1
        depth[d] = depth.get(d, 0) + 1
        per_folder[f["rel"].rsplit("/", 1)[0]] = per_folder.get(f["rel"].rsplit("/", 1)[0], 0) + 1
    s["depth"] = dict(sorted(depth.items()))
    counts = list(per_folder.values())
    s["audio_folders"] = len(counts)
    s["per_folder"] = {"mean": round(sum(counts) / max(1, len(counts)), 2), "p50": percentile(counts, 50),
                       "p90": percentile(counts, 90), "p99": percentile(counts, 99), "max": max(counts or [0]),
                       "single": sum(1 for c in counts if c == 1)}
    n = max(1, len(audio))

    def share(pred, pool=audio):
        return round(100.0 * sum(1 for f in pool if pred(f)) / max(1, len(pool)), 2)
    shapes = {}
    for f in audio:
        shapes[f["shape"]] = shapes.get(f["shape"], 0) + 1
    s["name_shapes"] = {k: round(100.0 * v / n, 2) for k, v in sorted(shapes.items(), key=lambda kv: -kv[1])}
    s["name_shapes_counts"] = dict(sorted(shapes.items(), key=lambda kv: -kv[1]))
    s["artist_in_name"] = share(lambda f: f["shape"] in ARTIST_SHAPES or f["title_rel"] == "artist")
    s["no_number"] = share(lambda f: f["shape"] in NUMBERLESS or (f["shape"] == "loose"))
    rels = {}
    for f in audio:
        rels[f["title_rel"]] = rels.get(f["title_rel"], 0) + 1
    s["title_vs_name"] = {k: round(100.0 * v / n, 2) for k, v in sorted(rels.items(), key=lambda kv: -kv[1])}
    albums = {}
    for f in audio:
        if f.get("album_id") is not None:
            albums.setdefault(f["album_id"], f["album_rel"])
    ar = {}
    for v in albums.values():
        ar[v] = ar.get(v, 0) + 1
    s["album_folders_vs_tag"] = {k: round(100.0 * v / max(1, len(albums)), 2) for k, v in sorted(ar.items(), key=lambda kv: -kv[1])}
    arts = [a for a in plan.artists if not a["va"]]
    art = {}
    for a in arts:
        art[a["rel"]] = art.get(a["rel"], 0) + 1
    s["artist_folders_vs_tag"] = {k: round(100.0 * v / max(1, len(arts)), 2) for k, v in sorted(art.items(), key=lambda kv: -kv[1])}
    s["tags"] = {
        "title": share(lambda f: f["tags"].get("title")),
        "artist": share(lambda f: f["tags"].get("artist")),
        "album": share(lambda f: f["tags"].get("album")),
        "track": share(lambda f: f["tags"].get("track")),
        "disc": share(lambda f: f["tags"].get("disc")),
        "disc_gt1": share(lambda f: (f["tags"].get("disc") or 0) > 1),
        "year": share(lambda f: f["tags"].get("year")),
        "genre": share(lambda f: f["tags"].get("genre")),
        "albumartist": share(lambda f: f["tags"].get("albumartist")),
        "compilation": share(lambda f: f["tags"].get("compilation")),
        "sort": share(lambda f: f["tags"].get("artistsort") or f["tags"].get("albumsort")),
        "multi_artist": share(lambda f: len(f["tags"].get("artist") or []) > 1),
        "multi_genre": share(lambda f: len(f["tags"].get("genre") or []) > 1),
        "feat_title": share(lambda f: "(feat. " in (f["tags"].get("title") or "")),
        "picture": share(lambda f: f.get("pic")),
        "picture_mp3": share(lambda f: f.get("pic"), [f for f in audio if f["kind"] == "mp3"]),
        "picture_flac": share(lambda f: f.get("pic"), [f for f in audio if f["kind"] == "flac"]),
        "rg": share(lambda f: f["tags"].get("rg_track")),
        "bpm": share(lambda f: f["tags"].get("bpm")),
        "mbid": share(lambda f: f["tags"].get("mbalbum")),
        "no_tags": share(lambda f: not f["tags"]),
        "non_latin_title": share(lambda f: any(ord(c) > 0x2FF for c in (f["tags"].get("title") or "")
                                               if c not in "\u2019")),
        "fat_unsafe_title": share(lambda f: any(c in FAT_BAD for c in (f["tags"].get("title") or ""))),
    }
    mp3 = [f for f in audio if f["kind"] == "mp3"]
    s["mp3_containers"] = {
        "id3v2.3": share(lambda f: not f["style"]["notags"] and not f["style"]["v1only"] and f["style"]["id3"] == "2.3", mp3),
        "id3v2.4": share(lambda f: not f["style"]["notags"] and not f["style"]["v1only"] and f["style"]["id3"] == "2.4", mp3),
        "id3v2.2": share(lambda f: not f["style"]["notags"] and not f["style"]["v1only"] and f["style"]["id3"] == "2.2", mp3),
        "id3v1": share(lambda f: not f["style"]["notags"] and f["style"]["v1"], mp3),
        "id3v1_only": share(lambda f: f["style"]["v1only"] and not f["style"]["notags"], mp3),
        "ape": share(lambda f: f["style"].get("ape") and not f["style"]["notags"], mp3),
        "unsync": share(lambda f: f["style"].get("unsync") and not f["style"]["notags"], mp3),
        "xing": share(lambda f: f["style"]["xing"], mp3),
        "lame": share(lambda f: f["style"]["lame"], mp3),
        "picture_before_text": share(lambda f: f["style"].get("pic_first") and f.get("pic"), mp3),
        "numeric_genre": share(lambda f: f["style"].get("numeric_genre") and f["tags"].get("genre"), mp3),
    }
    s["images"] = {"jpg": s["by_kind"]["jpg"], "progressive": sum(1 for f in files if f["kind"] == "jpg" and f.get("progressive")),
                   "png": s["by_kind"]["png"],
                   "named_cover_folders": len({f["rel"].rsplit("/", 1)[0] for f in files if f["kind"] == "jpg"
                                               and f["rel"].rsplit("/", 1)[1].lower() in ("cover.jpg", "folder.jpg", "front.jpg")})}
    s["other_files"] = {"text": s["by_kind"]["text"], "blob": s["by_kind"]["blob"], "hidden": s["by_kind"]["hidden"]}
    s["compilation_folders"] = sum(1 for a in plan.artists if a["va"])
    # The FAT layout: entries per folder (".", ".." included below /music's own), /music's sectors, sectors per open.
    children = {}
    for d in plan.folders:
        if "/" in d:
            parent, name = d.rsplit("/", 1)
            children.setdefault(parent, []).append(name)
    for f in files:
        parent, name = f["rel"].rsplit("/", 1)
        children.setdefault(parent, []).append(name)
    slots = {}
    entries = []
    for d in plan.folders:
        names = sorted(children.get(d, []), key=ntfs_key)
        pos = 2
        for nm in names:
            k = lfn_entries(nm)
            pos += k
            slots[(d, nm)] = pos - 1  # the SFN entry's slot (the last of its run)
        entries.append(pos)
    music_entries = entries[0]
    s["fat"] = {"music_entries": music_entries, "music_sectors": (music_entries * 32 + 511) // 512,
                "dir_entries": {"mean": round(sum(entries) / len(entries), 2), "p50": percentile(entries, 50),
                                "p90": percentile(entries, 90), "p99": percentile(entries, 99)}}
    opens = []
    in_music = []
    for f in audio:
        parts = f["rel"].split("/")
        sec = 0
        for i in range(1, len(parts)):
            d = "/".join(parts[:i])
            sec += slots[(d, parts[i])] // 16 + 1
            if i == 1:
                in_music.append(slots[(d, parts[i])] // 16 + 1)
        opens.append(sec)
    s["fat"]["open_sectors_mean"] = round(sum(opens) / max(1, len(opens)), 1)
    s["fat"]["open_sectors_music_mean"] = round(sum(in_music) / max(1, len(in_music)), 1)
    s["probes"] = []
    for p in getattr(plan, "probes", []):
        slot = slots[("music", p["artist"])]
        s["probes"].append(dict(p, slot=slot, sector=slot // 16))
    # What the device should see (firmware 0.7.0's walk and index).
    walk = [f for f in files if not f["rel"].rsplit("/", 1)[1].startswith(".") and
            len(f["rel"][len("music"):].encode("utf-8")) <= 255]
    tracks = [f for f in walk if f["rel"].lower().rsplit(".", 1)[-1] in AUDIO]
    art_with = set()
    alb = set()
    for f in tracks:
        parts = f["rel"].split("/")
        art_with.add(parts[1])
        alb.add((parts[1], parts[2] if len(parts) > 3 else ""))
    s["device"] = {"walk_files": len(walk), "tracks": len(tracks), "artists": len(art_with), "albums": len(alb),
                   "artists_az_first": [a for a in sorted(art_with, key=lambda x: (fold(x), x))][:4]}
    if sizes:
        total = sum(sizes.values())
        s["bytes"] = total
        s["stub_bytes"] = {k: _stats([sizes[f["rel"]] for f in files if f["kind"] == k]) for k in AUDIO}
        s["card"] = {}
        for c in (16384, 32768, 65536):
            data = sum((v + c - 1) // c * c for v in sizes.values())
            dirs = sum(max(1, (e * 32 + c - 1) // c) * c for e in entries)
            s["card"][f"{c // 1024}K"] = data + dirs
    return s


def _stats(xs):
    if not xs:
        return {}
    return {"n": len(xs), "mean": int(sum(xs) / len(xs)), "p50": percentile(xs, 50), "max": max(xs)}


def print_summary(s, out=sys.stdout):
    m = MEASURED
    w = out.write

    def rates(d):
        return ", ".join(f"{k} {v}%" for k, v in d.items())
    w(f"synthcard: seed {s['seed']}, {s['audio_files']:,} audio files ({s['count']:,} asked); measured: "
      f"{m['audio_files']:,}\n")
    k = s["by_kind"]
    w(f"  audio: MP3 {k['mp3']:,}, FLAC {k['flac']:,}, Opus {k['opus']:,} (--opus-share {s['opus_share']}%: the "
      f"library has none), not played {k['wma'] + k['m4a'] + k['mp4'] + k['ogg']} (.wma {k['wma']}, .m4a {k['m4a']}, "
      f".mp4 {k['mp4']}, .ogg {k['ogg']}); measured MP3 {m['mp3']:,}, FLAC {m['flac']:,}, not played 109\n")
    w(f"  artists {s['artists']} ({s['compilation_folders']} compilation folders); folders {s['folders']:,} (measured {m['folders']:,}); "
      f"with audio {s['audio_folders']:,} (measured {m['audio_folders']:,})\n")
    w("  depth (folders below /music): " + ", ".join(f"{d}: {n:,}" for d, n in s["depth"].items())
      + " (measured " + ", ".join(f"{d}: {n:,}" for d, n in m["depth"].items()) + ")\n")
    pf = s["per_folder"]
    w(f"  tracks per audio folder: mean {pf['mean']}, p50 {pf['p50']}, p90 {pf['p90']}, p99 {pf['p99']}, max "
      f"{pf['max']}, single-file {pf['single']} (measured mean 10.9, p50 11, p90 17, p99 28, max 43, 104)\n")
    w("  file names: " + ", ".join(f"{k} {v}%" for k, v in s["name_shapes"].items()) + "\n")
    w(f"    the artist in the name {s['artist_in_name']}% (measured 21.8-26.4%); no number {s['no_number']}% "
      f"(measured 8.9%)\n")
    w(f"  tag title vs the name's: {rates(s['title_vs_name'])} (measured same 61%)\n")
    w(f"  album folder vs the album tag: {rates(s['album_folders_vs_tag'])} (measured same 38, year 35, suffix 16, "
      f"other 4.5)\n")
    w(f"  artist folder vs the album artist else artist: {rates(s['artist_folders_vs_tag'])} (measured same 88 "
      f"(80 exact), case 9, other 3)\n")
    t = s["tags"]
    w(f"  tags: title {t['title']}%, artist {t['artist']}%, album {t['album']}%, track {t['track']}%, disc "
      f"{t['disc']}% (> 1: {t['disc_gt1']}%), year {t['year']}%, genre {t['genre']}%, album artist "
      f"{t['albumartist']}%, compilation {t['compilation']}%, sort {t['sort']}%, several artists "
      f"{t['multi_artist']}%, several genres {t['multi_genre']}%, feat. in the title {t['feat_title']}%, picture "
      f"{t['picture']}% (MP3 {t['picture_mp3']}%, FLAC {t['picture_flac']}%), ReplayGain {t['rg']}%, BPM "
      f"{t['bpm']}%, MBID {t['mbid']}%, none {t['no_tags']}%, non-Latin title {t['non_latin_title']}%, a FAT-unsafe "
      f"character in the title {t['fat_unsafe_title']}%\n")
    w(f"  MP3 containers: {rates(s['mp3_containers'])}\n")
    im = s["images"]
    w(f"  images: {im['jpg']:,} .jpg ({im['progressive']} progressive), {im['png']} .png, a named cover in "
      f"{im['named_cover_folders']:,} folders (measured 1,716 images, 647 named covers); other files "
      f"{s['other_files']['text'] + s['other_files']['blob']:,}, hidden {s['other_files']['hidden']} (measured "
      f"1,342 and 63)\n")
    f = s["fat"]
    w(f"  FAT: /music {f['music_entries']:,} entries in {f['music_sectors']} sectors (measured {m['music_fat_entries']:,} "
      f"in {m['music_sectors']}); entries per folder mean {f['dir_entries']['mean']}, p50 {f['dir_entries']['p50']}, "
      f"p90 {f['dir_entries']['p90']}, p99 {f['dir_entries']['p99']} (measured 37.95/35/73/124); directory sectors "
      f"per open {f['open_sectors_mean']}, {f['open_sectors_music_mean']} of them in /music (measured 56.2 and 51.3)\n")
    d = s["device"]
    w(f"  the device should walk {d['walk_files']:,} files and index {d['tracks']:,} tracks, {d['artists']} "
      f"artists, {d['albums']:,} albums; A-Z first: {', '.join(d['artists_az_first'])}\n")
    for p in s["probes"]:
        w(f"  probe at /music entry {p['position']} (slot {p['slot']}, sector {p['sector']}): {p['artist']}/"
          f"{p['album']} ({p['tracks']} tracks)\n")
    if "bytes" in s:
        sb = s["stub_bytes"]
        w(f"  size: {s['bytes'] / 1e6:.1f} MB in {sum(s['by_kind'].values()):,} files (stubs: MP3 mean "
          f"{sb['mp3'].get('mean', 0):,} B, FLAC {sb['flac'].get('mean', 0):,} B, Opus {sb['opus'].get('mean', 0):,} B); "
          f"on a FAT32 card " + ", ".join(f"{v / 1e6:.0f} MB at {c} clusters" for c, v in s["card"].items()) + "\n")


# ---------------------------------------------------------------------------
# Writing the tree
# ---------------------------------------------------------------------------


def long_path(p):
    """Windows' extended-length form (a deep tree under a long folder)."""
    p = os.path.abspath(p)
    if os.name == "nt" and not p.startswith("\\\\?\\"):
        return "\\\\?\\" + p
    return p


MARKER = "synthcard.json"


def check_out_dir(out, probe=None):
    """The output folder must be local and empty, or one this tool wrote."""
    probe = probe or DriveProbe()
    drive = os.path.splitdrive(os.path.abspath(out))[0]
    if drive and probe.removable(drive + "\\"):
        raise SystemExit(f"--out {out}: that is a removable drive; the tree is built in a local folder (use "
                         f"--copy-to to put it on a card)")
    if os.path.isdir(out) and os.listdir(out) and not os.path.isfile(os.path.join(out, MARKER)):
        raise SystemExit(f"--out {out}: not empty and not a tree this tool wrote (no {MARKER}): refusing to touch it")


def write_tree(plan, out, progress=True):
    """Writes the plan under out; returns {rel: size}."""
    if os.path.isdir(os.path.join(out, "music")):
        shutil.rmtree(long_path(os.path.join(out, "music")))
    os.makedirs(out, exist_ok=True)
    sizes = {}
    expect = []
    for d in plan.folders:
        os.makedirs(long_path(os.path.join(out, *d.split("/"))), exist_ok=True)
    t0 = time.time()
    last = 0
    n = len(plan.files)
    for i, f in enumerate(plan.files):
        data, ms, pic, written = file_bytes(f)
        p = long_path(os.path.join(out, *f["rel"].split("/")))
        with open(p, "wb") as fh:
            fh.write(data)
        os.utime(p, (f["mtime"], f["mtime"]))
        sizes[f["rel"]] = len(data)
        if f["kind"] in AUDIO:
            expect.append(_expect_row(f, len(data), ms, pic, written))
        if progress and time.time() - last > 0.5:
            last = time.time()
            sys.stdout.write(f"\r  writing {i + 1:,}/{n:,} files ({(time.time() - t0):.0f} s)")
            sys.stdout.flush()
    if progress:
        sys.stdout.write(f"\r  wrote {n:,} files in {time.time() - t0:.0f} s{' ' * 20}\n")
    with open(os.path.join(out, "synthcard-files.jsonl"), "w", encoding="utf-8") as fh:
        for row in expect:
            fh.write(json.dumps(row, ensure_ascii=False, sort_keys=True) + "\n")
    # Folder times last (writing their files moved them).
    for d in reversed(plan.folders):
        p = long_path(os.path.join(out, *d.split("/")))
        t = 1262347200 + (zlib.crc32(d.encode("utf-8")) % 300000000) // 2 * 2
        os.utime(p, (t, t))
    return sizes


def _expect_row(f, size, ms, pic, written):
    st = f["style"]
    row = {"path": f["rel"], "kind": f["kind"], "bytes": size, "ms": ms, "tags": f["tags"], "shape": f["shape"],
           "title_rel": f["title_rel"], "pic": pic}
    if f["kind"] == "mp3":
        row["id3"] = None if st["notags"] or st["v1only"] else st["id3"]
        row["v1"] = bool(written.get("v1"))
        row["ape"] = bool(written.get("ape"))
        row["unsync"] = bool(st.get("unsync")) and not st["notags"] and not st["v1only"]
        row["xing"] = st["xing"]
        row["lame"] = st["lame"]
        row["numeric_genre"] = bool(st.get("numeric_genre"))
    return row


def sample_files(plan, n, seed=0):
    """A spread of n audio files for the parse checks: every container
    variant first, then a random fill."""
    rng = random.Random(seed)
    audio = [f for f in plan.files if f["kind"] in AUDIO]
    keys = {}
    for f in audio:
        st = f["style"]
        k = (f["kind"], st.get("id3"), st.get("v1only"), st.get("notags"), bool(st.get("unsync")), bool(f.get("pic")),
             f.get("pic"), st.get("xing"), st.get("lame"), st.get("ape"), st.get("enc"), st.get("pic_first"),
             len(f["tags"].get("artist") or []) > 1, len(f["tags"].get("genre") or []) > 1)
        keys.setdefault(k, f)
    out = list(keys.values())
    rest = [f for f in audio if f not in out]
    out += rng.sample(rest, max(0, min(len(rest), n - len(out))))
    return out


# ---------------------------------------------------------------------------
# Copying to a card (Windows)
# ---------------------------------------------------------------------------


# What an empty card's root may hold: the folder Windows itself makes on a
# drive it mounts (the indexer's volume id). Anything else there: refused.
ROOT_IGNORED = {"system volume information"}


class DriveProbe:
    """What Windows says about a drive. The tests replace it."""

    def _k32(self):
        import ctypes
        return ctypes.windll.kernel32

    def removable(self, root):
        if os.name != "nt":
            return False
        return self._k32().GetDriveTypeW(root) == 2  # DRIVE_REMOVABLE

    def system_drive(self):
        return (os.environ.get("SystemDrive", "C:") + "\\").upper()

    def info(self, root):
        """{"type", "label", "fs", "size", "free", "cluster"}"""
        import ctypes
        k = self._k32()
        label = ctypes.create_unicode_buffer(261)
        fs = ctypes.create_unicode_buffer(261)
        ok = k.GetVolumeInformationW(ctypes.c_wchar_p(root), label, 261, None, None, None, fs, 261)
        free, total = ctypes.c_ulonglong(0), ctypes.c_ulonglong(0)
        k.GetDiskFreeSpaceExW(ctypes.c_wchar_p(root), ctypes.byref(free), ctypes.byref(total), None)
        spc, bps, nf, tc = (ctypes.c_ulong(0) for _ in range(4))
        k.GetDiskFreeSpaceW(ctypes.c_wchar_p(root), ctypes.byref(spc), ctypes.byref(bps), ctypes.byref(nf),
                            ctypes.byref(tc))
        types = {0: "unknown", 1: "no root", 2: "removable", 3: "fixed", 4: "network", 5: "CD-ROM", 6: "RAM disk"}
        return {"type": types.get(k.GetDriveTypeW(root), "?"), "label": label.value if ok else "", "fs": fs.value if ok else "",
                "size": total.value, "free": free.value, "cluster": spc.value * bps.value, "readable": bool(ok)}

    def target(self, root, rel):
        return os.path.join(root, *rel.split("/"))


def copy_to(out, drive, yes, probe=None, stream=sys.stdout):
    """--copy-to: checks, says what it would do, copies only with yes.
    Returns 0 copied, 2 not asked (no --yes), raises SystemExit on a refusal:
    a drive that isn't removable, FAT32 and empty is never written to."""
    probe = probe or DriveProbe()
    w = stream.write
    d = drive.strip()
    if len(d) < 2 or d[1] != ":" or not d[0].isalpha() or d[2:] not in ("", "\\", "/"):
        raise SystemExit(f"--copy-to {drive}: a drive's root, like E:\\")
    root = d[0].upper() + ":\\"
    if os.name != "nt" and type(probe) is DriveProbe:
        raise SystemExit("--copy-to works on Windows only (it asks Windows what the drive is)")
    manifest = os.path.join(out, MARKER)
    if not os.path.isfile(manifest):
        raise SystemExit(f"{out}: no {MARKER}: build the tree first (python tools/synthcard.py --out {out})")
    with open(manifest, encoding="utf-8") as fh:
        summ = json.load(fh)
    if root.upper() == probe.system_drive().upper():
        raise SystemExit(f"--copy-to {root}: that is the system drive: refusing")
    info = probe.info(root)
    w(f"drive {root}: label \"{info['label']}\", {info['type']}, {info['fs'] or 'no file system read'}, "
      f"{info['size'] / 1e9:.2f} GB, {info['free'] / 1e9:.2f} GB free, {info['cluster'] // 1024} KB clusters\n")
    if info["type"] != "removable":
        raise SystemExit(f"--copy-to {root}: not a removable drive ({info['type']}): refusing")
    if info["fs"].upper() != "FAT32":
        raise SystemExit(f"--copy-to {root}: {info['fs'] or 'no file system'}, not FAT32 (the firmware reads FAT32 "
                         f"only): refusing; the tool never formats a card")
    if os.path.exists(probe.target(root, "music")):
        raise SystemExit(f"--copy-to {root}: the card already has \\music: refusing (this tool only fills an empty "
                         f"card; it never deletes or formats)")
    try:
        present = sorted(n for n in os.listdir(probe.target(root, "")) if n.casefold() not in ROOT_IGNORED)
    except OSError as e:
        raise SystemExit(f"--copy-to {root}: its root can't be listed ({e}): refusing")
    if present:
        shown = ", ".join(present[:5]) + (f" and {len(present) - 5} more" if len(present) > 5 else "")
        raise SystemExit(f"--copy-to {root}: the card isn't empty ({shown}): refusing (this tool only fills an empty "
                         f"card; it never deletes or formats: empty or format it yourself first)")
    ops = copy_plan(out)
    files = [(rel, sz) for kind, rel, sz in ops if kind == "file"]
    dirs = [rel for kind, rel, _ in ops if kind == "dir"]
    c = info["cluster"] or 32768
    # Each folder's entries ("." and ".." and the LFN runs) in whole clusters: /music's 1,600-odd take two.
    entries = {d: 2 for d in dirs}
    for _, rel, _ in ops:
        if "/" in rel:
            parent, name = rel.rsplit("/", 1)
            entries[parent] += lfn_entries(name)
    need = sum((sz + c - 1) // c * c for _, sz in files) + sum((e * 32 + c - 1) // c * c for e in entries.values())
    w(f"would copy {len(files):,} files ({sum(sz for _, sz in files) / 1e6:.1f} MB; about {need / 1e6:.0f} MB on "
      f"this card's clusters) and {len(dirs):,} folders from {out} to {root}: \\music\\ "
      f"({summ.get('artists', 0)} artist folders, created in name order) and \\SYNTHCARD.TXT\n")
    if need > info["free"]:
        raise SystemExit(f"--copy-to {root}: {need / 1e6:.0f} MB needed, {info['free'] / 1e6:.0f} MB free: refusing")
    if not yes:
        w("nothing copied: run again with --yes to copy\n")
        return 2
    t0 = time.time()
    done_b = done_n = 0
    last = 0
    for kind, rel, sz in ops:
        dst = long_path(probe.target(root, rel))
        if kind == "dir":
            os.mkdir(dst)  # (never exists: \music didn't)
            continue
        shutil.copy2(long_path(os.path.join(out, *rel.split("/"))), dst)
        done_b += sz
        done_n += 1
        now = time.time()
        if now - last > 0.5 or done_n == len(files):
            last = now
            rate = done_n / max(0.001, now - t0)
            eta = (len(files) - done_n) / max(rate, 0.001)
            w(f"\r  copied {done_n:,}/{len(files):,} files, {done_b / 1e6:.1f} MB, {rate:.0f} files/s, "
              f"about {eta / 60:.0f} min left   ")
            if hasattr(stream, "flush"):
                stream.flush()
    # The folders' times as the tree has them (made, then filled: the copy moved them).
    for rel in reversed(dirs):
        t = os.stat(long_path(os.path.join(out, *rel.split("/")))).st_mtime
        os.utime(long_path(probe.target(root, rel)), (t, t))
    w(f"\n  done in {(time.time() - t0) / 60:.1f} min. Eject the card safely before taking it out.\n")
    return 0


def copy_plan(out):
    """The copy, in order: [("dir"|"file", rel, size)]. Pre-order, each
    folder before what it holds, names in NTFS's order, so the card's
    directory entries come in the order the summary's FAT figures and the
    probes' positions assume."""
    ops = [("file", "SYNTHCARD.TXT", os.path.getsize(os.path.join(out, "SYNTHCARD.TXT")))] \
        if os.path.isfile(os.path.join(out, "SYNTHCARD.TXT")) else []

    def walk(rel):
        ops.append(("dir", rel, 0))
        full = long_path(os.path.join(out, *rel.split("/")))
        names = sorted(os.listdir(full), key=ntfs_key)
        for nm in names:
            p = os.path.join(full, nm)
            if os.path.isdir(p):
                walk(f"{rel}/{nm}")
            else:
                ops.append(("file", f"{rel}/{nm}", os.path.getsize(p)))
    walk("music")
    return ops


# ---------------------------------------------------------------------------


def default_out():
    return os.environ.get("SYNTHCARD_OUT") or os.path.join(tempfile.gettempdir(), "synthcard")


def build(out, count, seed, opus_share, progress=True):
    plan = make_plan(count, seed, opus_share)
    check_out_dir(out)
    sizes = write_tree(plan, out, progress)
    summ = summary(plan, sizes)
    with open(os.path.join(out, MARKER), "w", encoding="utf-8") as fh:
        json.dump(summ, fh, indent=1, ensure_ascii=False)
    with open(os.path.join(out, "SYNTHCARD.TXT"), "w", encoding="utf-8", newline="\r\n") as fh:
        fh.write(f"A synthetic library made by tools/synthcard.py (mstream-mp3-player), seed {seed}, "
                 f"{summ['audio_files']} audio files.\nEvery name is made up and every track is silence.\n"
                 f"It is a test card for the Core2's big-card checks (docs/METADATA.md, L0): delete it when done.\n")
    return plan, summ


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=None, help="the local folder for the tree (default: $SYNTHCARD_OUT, else "
                    "<the system temp folder>/synthcard)")
    ap.add_argument("--count", type=int, default=MEASURED["audio_files"], help="audio files (default 19,519)")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--opus-share", type=float, default=1.0, help="percent of the audio files as Opus (default 1)")
    ap.add_argument("--plan", action="store_true", help="the summary only (the sizes too): nothing written")
    ap.add_argument("--json", action="store_true", help="with --plan: the summary as JSON")
    ap.add_argument("--copy-to", metavar="DRIVE", help="copy the built tree in --out to this card's root (E:\\): an "
                    "empty, removable FAT32 drive only")
    ap.add_argument("--yes", action="store_true", help="with --copy-to: copy (without it, only say what it would do)")
    a = ap.parse_args(argv)
    if a.count < 50:
        ap.error("--count: at least 50")
    out = a.out or default_out()
    if a.copy_to:
        return copy_to(out, a.copy_to, a.yes)
    if a.plan:
        plan = make_plan(a.count, a.seed, a.opus_share)
        sizes = {}
        for f in plan.files:
            data = file_bytes(f)[0]
            sizes[f["rel"]] = len(data)
        s = summary(plan, sizes)
        if a.json:
            print(json.dumps(s, indent=1, ensure_ascii=False))
        else:
            print_summary(s)
        return 0
    print(f"building the tree in {out} (seed {a.seed}, {a.count:,} audio files)")
    plan, s = build(out, a.count, a.seed, a.opus_share)
    print_summary(s)
    print(f"  written: {out}\\music, {out}\\{MARKER}, {out}\\synthcard-files.jsonl, {out}\\SYNTHCARD.TXT")
    return 0


if __name__ == "__main__":
    sys.exit(main())
