// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

//! The reference reader of the tag parity corpus (docs/METADATA.md 2.17,
//! item 3): each file of test/fixtures/tags read the way the transfer
//! software's reader is specified, lofty 0.25 in relaxed mode behind
//! mStream's ID3v2 repair pass, with mStream's rust-parser selection rules
//! (READ at mStream 926b97b1) and part 5's rules on top, into the record of
//! 2.6.4 and 2.6.5. Its output is test/fixtures/tags/expected.json, which the
//! player's TagScan must match (test/test_tag_scan): two readers agreeing.
//!
//! Written for this repo from the rules, not copied from mStream: the repair
//! pass's six rules, the tag choice, the ID3v1 fill, the field rules of 5.3,
//! the number rule, 2.3.6. The picture's anchor (2.6.4) isn't something
//! lofty reports: the elected picture is found in test/fixtures/tags/
//! anchors.json by its data's FNV-1a 64.
//!
//!     cargo run --manifest-path tools/tagref/Cargo.toml            write expected.json
//!     cargo run --manifest-path tools/tagref/Cargo.toml -- --check fail if it differs
//!
//! (run from the repo root; lofty 0.25.1 and serde_json from the registry).

use std::collections::BTreeMap;
use std::io::Cursor;
use std::path::{Path, PathBuf};

use lofty::config::{ParseOptions, ParsingMode};
use lofty::file::{AudioFile, TaggedFile, TaggedFileExt};
use lofty::id3::v2::{Frame, Id3v2Tag};
use lofty::picture::{MimeType, Picture, PictureType, APE_PICTURE_TYPES};
use lofty::tag::{Accessor, ItemKey, ItemValue, Tag, TagType};
use serde_json::{json, Value};

// ---------------------------------------------------------------------------
// mStream's ID3v2 repair pass (5.2), for the tag at the file's start: a
// rewrite of that tag lofty then reads in its place, the same length.
//   1. v2.4 unsynchronisation (the frame's flag, or the tag's for every
//      frame) undone frame by frame; v2.2/2.3 tag-level unsynchronisation
//      undone for the walk and the flag cleared.
//   2. a UTF-16 text frame of odd length: the stray byte becomes U+FFFD.
//   3. a UTF-8 text frame that isn't: lossy, U+FFFD per invalid subpart.
//   4. a frame running past the tag's end: cut there, nothing after it.
//   5. a URL frame (W***, not WXXX) with a NUL in front: the NUL dropped.
//   6. a v2.4 frame size that isn't syncsafe: read as a plain integer when
//      that, and not the syncsafe reading, lands on a frame boundary.
// When the grown rewrite doesn't fit the tag, the pass shrinks instead
// (the stray byte dropped, '?' per bad byte); a tag with an extended
// header, or a v2.4 unsynchronised frame shorter than its flagged lead
// bytes, isn't rewritten.
// ---------------------------------------------------------------------------
const MAX_REPAIR: usize = 64 * 1024 * 1024;

fn syncsafe(b: &[u8]) -> usize {
    ((b[0] & 0x7f) as usize) << 21 | ((b[1] & 0x7f) as usize) << 14 | ((b[2] & 0x7f) as usize) << 7 | (b[3] & 0x7f) as usize
}

fn deunsync(src: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(src.len());
    let mut i = 0;
    while i < src.len() {
        out.push(src[i]);
        if src[i] == 0xff && src.get(i + 1) == Some(&0) {
            i += 1;
        }
        i += 1;
    }
    out
}

fn text_frame_id(id: &[u8]) -> bool {
    id.first() == Some(&b'T') || matches!(id, b"COMM" | b"USLT" | b"IPLS" | b"COM" | b"ULT" | b"IPL")
}

fn id_byte(b: u8) -> bool {
    b.is_ascii_uppercase() || b.is_ascii_digit()
}

fn boundary_ok(data: &[u8], at: usize) -> bool {
    at == data.len() || (at < data.len() && (data[at] == 0 || (at + 4 <= data.len() && data[at..at + 4].iter().all(|b| id_byte(*b)))))
}

fn repair_pass(bytes: &[u8], grow: bool) -> Option<Vec<u8>> {
    if bytes.len() < 10 || &bytes[..3] != b"ID3" || !(2..=4).contains(&bytes[3]) {
        return None;
    }
    let major = bytes[3];
    let flags = bytes[5];
    if flags & 0x40 != 0 {
        return None;
    }
    let size = syncsafe(&bytes[6..10]);
    if size == 0 || size > MAX_REPAIR {
        return None;
    }
    let raw = &bytes[10..bytes.len().min(10 + size)];
    let tag_unsync = flags & 0x80 != 0;
    let whole;
    let data: &[u8] = if tag_unsync && major < 4 {
        whole = deunsync(raw);
        &whole
    } else {
        raw
    };
    let (hl, idl) = if major == 2 { (6, 3) } else { (10, 4) };
    let mut out: Option<Vec<u8>> = None;
    let mut pos = 0;
    while pos + hl <= data.len() {
        if data[pos] == 0 {
            break;
        }
        let id = &data[pos..pos + idl];
        let mut plain = false;
        let declared = match major {
            2 => (data[pos + 3] as usize) << 16 | (data[pos + 4] as usize) << 8 | data[pos + 5] as usize,
            3 => u32::from_be_bytes([data[pos + 4], data[pos + 5], data[pos + 6], data[pos + 7]]) as usize,
            _ => {
                let b = &data[pos + 4..pos + 8];
                let ss = syncsafe(b);
                let be = u32::from_be_bytes([b[0], b[1], b[2], b[3]]) as usize;
                if be == ss {
                    ss
                } else {
                    let ok_ss = b.iter().all(|x| x & 0x80 == 0) && boundary_ok(data, pos + hl + ss);
                    let ok_be = boundary_ok(data, pos + hl + be);
                    if !ok_ss && ok_be {
                        plain = true;
                        be
                    } else {
                        ss
                    }
                }
            }
        };
        let fflags = if major == 2 { 0 } else { u16::from_be_bytes([data[pos + 8], data[pos + 9]]) };
        let body_at = pos + hl;
        let truncated = declared > data.len() - body_at;
        let core = if major == 3 && id[3] == 0 { &id[..3] } else { id };
        if !core.iter().all(|b| id_byte(*b)) {
            if truncated {
                break;
            }
            if let Some(o) = out.as_mut() {
                o.extend_from_slice(&data[pos..body_at + declared]);
            }
            pos = body_at + declared;
            continue;
        }
        let raw_body = &data[body_at..(body_at + declared).min(data.len())];
        let (compressed, encrypted, grouping, frame_unsync, dli) = match major {
            3 => (fflags & 0x80 != 0, fflags & 0x40 != 0, fflags & 0x20 != 0, false, false),
            4 => (fflags & 0x08 != 0, fflags & 0x04 != 0, fflags & 0x40 != 0, fflags & 0x02 != 0, fflags & 0x01 != 0),
            _ => (false, false, false, false, false),
        };
        let mut new_flags = fflags;
        let mut changed = truncated || plain;
        let mut body: Vec<u8>;
        if major == 4 && (frame_unsync || tag_unsync) {
            let lead = encrypted as usize + grouping as usize + if dli || compressed { 4 } else { 0 };
            if raw_body.len() < lead {
                return None;
            }
            body = raw_body[..lead].to_vec();
            body.extend(deunsync(&raw_body[lead..]));
            new_flags &= !0x0002;
            changed = true;
        } else {
            body = raw_body.to_vec();
        }
        if !compressed && !encrypted && id[0] == b'W' && id != b"WXXX" && id != b"WXX" {
            let lead = grouping as usize + if major == 4 && dli { 4 } else { 0 };
            if body.len() > lead + 1 && body[lead] == 0 {
                body.remove(lead);
                changed = true;
            }
        }
        if !compressed && !encrypted && text_frame_id(id) {
            let lead = grouping as usize + if major == 4 && dli { 4 } else { 0 };
            let lang = if matches!(id, b"COMM" | b"USLT" | b"COM" | b"ULT") { 3 } else { 0 };
            let text = lead + 1 + lang;
            if body.len() > text {
                match body[lead] {
                    e @ (1 | 2) if (body.len() - text) % 2 == 1 => {
                        body.pop();
                        if grow {
                            let be = e == 2 || body[text..].starts_with(&[0xfe, 0xff]);
                            body.extend(if be { [0xff, 0xfd] } else { [0xfd, 0xff] });
                        }
                        changed = true;
                    }
                    3 if std::str::from_utf8(&body[text..]).is_err() => {
                        let fixed: Vec<u8> = if grow {
                            String::from_utf8_lossy(&body[text..]).into_owned().into_bytes()
                        } else {
                            body[text..]
                                .utf8_chunks()
                                .flat_map(|c| c.valid().bytes().chain(std::iter::repeat(b'?').take(c.invalid().len())).collect::<Vec<u8>>())
                                .collect()
                        };
                        body.truncate(text);
                        body.extend(fixed);
                        changed = true;
                    }
                    _ => {}
                }
            }
        }
        if changed && out.is_none() {
            let mut o = Vec::with_capacity(10 + size);
            o.extend_from_slice(&bytes[..10]);
            o[5] = flags & !0x80;
            o.extend_from_slice(&data[..pos]);
            out = Some(o);
        }
        if let Some(o) = out.as_mut() {
            o.extend_from_slice(id);
            let n = body.len();
            match major {
                2 => o.extend_from_slice(&[(n >> 16) as u8, (n >> 8) as u8, n as u8]),
                3 => o.extend_from_slice(&(n as u32).to_be_bytes()),
                _ => o.extend_from_slice(&[((n >> 21) & 0x7f) as u8, ((n >> 14) & 0x7f) as u8, ((n >> 7) & 0x7f) as u8, (n & 0x7f) as u8]),
            }
            if major != 2 {
                o.extend_from_slice(&new_flags.to_be_bytes());
            }
            o.extend_from_slice(&body);
        }
        if truncated {
            break;
        }
        pos = body_at + declared;
    }
    out
}

/// The file's bytes as lofty is given them: the first tag repaired in place.
fn repaired(file: &[u8]) -> Vec<u8> {
    let mut data = file.to_vec();
    if file.len() < 10 || &file[..3] != b"ID3" {
        return data;
    }
    let full = file.len().min(10 + syncsafe(&file[6..10]));
    let Some(mut out) = repair_pass(file, true) else { return data };
    if out.len() > full {
        match repair_pass(file, false) {
            Some(o) if o.len() <= full => out = o,
            _ => return data,
        }
    }
    out.resize(full, 0);
    data[..full].copy_from_slice(&out);
    data
}

// ---------------------------------------------------------------------------
// Reading: lofty's file reader by the extension (mStream's read_tagged), and
// what the generic Tag doesn't carry: TPE2's values, the TXXX descriptions,
// the ID3v2 pictures with their frames' flags, every APE cover item.
// ---------------------------------------------------------------------------
struct Probed {
    file: TaggedFile,
    parts: Parts,
}

#[derive(Default)]
struct Parts {
    tpe2: Option<Vec<String>>,
    custom: Vec<(String, String)>,
    id3_pictures: Vec<Picture>,
    ape_pictures: Vec<Picture>,
}

fn id3_parts(tag: Option<&Id3v2Tag>, p: &mut Parts) {
    let Some(tag) = tag else { return };
    for frame in tag {
        match frame {
            Frame::Text(f) if frame.id().as_str() == "TPE2" => {
                p.tpe2 = Some(f.value.split('\0').filter(|v| !v.is_empty()).map(str::to_string).collect());
            }
            Frame::UserText(f) => p.custom.push((f.description.to_ascii_uppercase(), f.content.to_string())),
            // 5.3: a compressed or encrypted frame's picture is never elected
            // (an encrypted one isn't a picture to lofty anyway).
            Frame::Picture(f) if !frame.flags().compression && frame.flags().encryption.is_none() => {
                p.id3_pictures.push(f.picture.clone().into_owned());
            }
            _ => {}
        }
    }
}

fn ape_parts(tag: Option<&lofty::ape::ApeTag>, p: &mut Parts) {
    let Some(tag) = tag else { return };
    for item in tag {
        let Some(canonical) = APE_PICTURE_TYPES.iter().find(|k| k.eq_ignore_ascii_case(item.key())) else { continue };
        if let ItemValue::Binary(data) = item.value() {
            if let Ok(pic) = Picture::from_ape_bytes(canonical, data) {
                p.ape_pictures.push(pic);
            }
        }
    }
}

fn read(bytes: &[u8], ext: &str) -> Result<Probed, String> {
    let opts = ParseOptions::new().parsing_mode(ParsingMode::Relaxed);
    let data = repaired(bytes);
    let mut cur = Cursor::new(data);
    let mut parts = Parts::default();
    let file: TaggedFile = match ext {
        "mp3" => {
            let f = lofty::mpeg::MpegFile::read_from(&mut cur, opts).map_err(|e| e.to_string())?;
            id3_parts(f.id3v2(), &mut parts);
            ape_parts(f.ape(), &mut parts);
            f.into()
        }
        "flac" => {
            let f = lofty::flac::FlacFile::read_from(&mut cur, opts).map_err(|e| e.to_string())?;
            id3_parts(f.id3v2(), &mut parts);
            f.into()
        }
        "opus" => lofty::ogg::OpusFile::read_from(&mut cur, opts).map_err(|e| e.to_string())?.into(),
        _ => return Err("not an audio extension".into()),
    };
    Ok(Probed { file, parts })
}

// ---------------------------------------------------------------------------
// 2.3.6: control characters to spaces, empty values dropped, each cut to 255
// bytes at a code point, repeats dropped, the list limits.
// ---------------------------------------------------------------------------
fn clean(v: &str) -> String {
    v.chars().map(|c| if (c as u32) < 0x20 || c as u32 == 0x7f { ' ' } else { c }).collect()
}

fn cut255(v: &str) -> (String, bool) {
    if v.len() <= 255 {
        return (v.to_string(), false);
    }
    let mut end = 255;
    while !v.is_char_boundary(end) {
        end -= 1;
    }
    (v[..end].to_string(), true)
}

fn list(values: &[String], truncated: &mut bool) -> String {
    let mut out: Vec<String> = Vec::new();
    let mut len = 0;
    for v in values {
        let v = clean(v);
        if v.is_empty() {
            continue;
        }
        let (v, cut) = cut255(&v);
        *truncated |= cut;
        if out.contains(&v) {
            continue;
        }
        let grown = len + if out.is_empty() { 0 } else { 1 } + v.len();
        if out.len() + 1 > 16 || grown > 1023 {
            *truncated = true;
            break;
        }
        len = grown;
        out.push(v);
    }
    out.join("\u{1f}")
}

fn single(values: &[String], truncated: &mut bool) -> String {
    for v in values {
        let v = clean(v);
        if v.is_empty() {
            continue;
        }
        let (v, cut) = cut255(&v);
        *truncated |= cut;
        return v;
    }
    String::new()
}

// ---------------------------------------------------------------------------
// 5.3's rules
// ---------------------------------------------------------------------------
/// The number rule: trim ASCII whitespace (a gain: one trailing "dB" in any
/// case, then trim again); [+-]?([0-9]+(\.[0-9]*)?|\.[0-9]+); exact decimal at
/// `scale` fraction digits, half away from zero. None when it doesn't match;
/// a magnitude too large saturates at i64's.
fn decimal(s: &str, scale: usize, gain: bool) -> Option<i64> {
    let ws = |c: char| c.is_ascii_whitespace();
    let mut t = s.trim_matches(ws);
    if gain && t.len() >= 2 && t.is_char_boundary(t.len() - 2) && t[t.len() - 2..].eq_ignore_ascii_case("db") {
        t = t[..t.len() - 2].trim_matches(ws);
    }
    let (neg, body) = match t.as_bytes().first() {
        Some(b'+') => (false, &t[1..]),
        Some(b'-') => (true, &t[1..]),
        _ => (false, t),
    };
    let (int, frac) = match body.find('.') {
        Some(i) => (&body[..i], &body[i + 1..]),
        None => (body, ""),
    };
    if !int.bytes().all(|b| b.is_ascii_digit()) || !frac.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    if int.is_empty() && frac.is_empty() {
        return None;
    }
    let mut mag: i64 = 0;
    let push = |m: i64, d: u8| m.saturating_mul(10).saturating_add((d - b'0') as i64);
    for d in int.bytes() {
        mag = push(mag, d);
    }
    let fb = frac.as_bytes();
    for k in 0..scale {
        mag = push(mag, *fb.get(k).unwrap_or(&b'0'));
    }
    if fb.get(scale).map_or(false, |d| *d >= b'5') {
        mag = mag.saturating_add(1);
    }
    Some(if neg { -mag } else { mag })
}

fn bpm10(s: &str) -> u16 {
    match decimal(s, 0, false) {
        Some(n) if (20..=300).contains(&n) => (n * 10) as u16,
        _ => 0,
    }
}

fn gain(s: &str) -> Option<i16> {
    decimal(s, 2, true).filter(|v| (i16::MIN as i64..=i16::MAX as i64).contains(v)).map(|v| v as i16)
}

fn peak(s: &str) -> u16 {
    match decimal(s, 4, false) {
        Some(v) if v >= 0 => v.min(65535) as u16,
        _ => 0,
    }
}

/// An Opus R128 gain: an integer [+-]?[0-9]+ (ASCII whitespace trimmed, as
/// the number rule trims) in the i16 range.
fn r128(s: &str) -> Option<i16> {
    let s = s.trim_matches(|c: char| c.is_ascii_whitespace());
    let t = s.strip_prefix(['+', '-']).unwrap_or(s);
    if t.is_empty() || !t.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    let q: i64 = s.parse().unwrap_or(i64::MAX);
    if !(i16::MIN as i64..=i16::MAX as i64).contains(&q) {
        return None;
    }
    // (q / 256 + 5) x 100, in integers: (q + 1280) x 25 / 64, half away from zero.
    let n = (q + 1280) * 25;
    let r = if n >= 0 { (n + 32) / 64 } else { -((-n + 32) / 64) };
    Some(r as i16)
}

fn compilation(s: &str) -> u8 {
    if s == "1" || s.eq_ignore_ascii_case("true") {
        1
    } else if s == "0" || s.eq_ignore_ascii_case("false") {
        2
    } else {
        0
    }
}

const CAMELOT: [&[&str]; 24] = [
    &["1A", "Ab minor", "Abmin", "G# minor", "G#min", "Abm", "G#m"],
    &["1B", "B major", "Bmaj", "B"],
    &["2A", "Eb minor", "Ebmin", "D# minor", "D#min", "Ebm", "D#m"],
    &["2B", "F# major", "F#maj", "Gb major", "Gbmaj", "F#", "Gb"],
    &["3A", "Bb minor", "Bbmin", "A# minor", "A#min", "Bbm", "A#m"],
    &["3B", "Db major", "Dbmaj", "C# major", "C#maj", "Db", "C#"],
    &["4A", "F minor", "Fmin", "Fm"],
    &["4B", "Ab major", "Abmaj", "G# major", "G#maj", "Ab", "G#"],
    &["5A", "C minor", "Cmin", "Cm"],
    &["5B", "Eb major", "Ebmaj", "D# major", "D#maj", "Eb", "D#"],
    &["6A", "G minor", "Gmin", "Gm"],
    &["6B", "Bb major", "Bbmaj", "A# major", "A#maj", "Bb", "A#"],
    &["7A", "D minor", "Dmin", "Dm"],
    &["7B", "F major", "Fmaj", "F"],
    &["8A", "A minor", "Amin", "Am"],
    &["8B", "C major", "Cmaj", "C"],
    &["9A", "E minor", "Emin", "Em"],
    &["9B", "G major", "Gmaj", "G"],
    &["10A", "B minor", "Bmin", "Bm"],
    &["10B", "D major", "Dmaj", "D"],
    &["11A", "F# minor", "F#min", "Gb minor", "Gbmin", "F#m", "Gbm"],
    &["11B", "A major", "Amaj", "A"],
    &["12A", "C# minor", "C#min", "Db minor", "Dbmin", "C#m", "Dbm"],
    &["12B", "E major", "Emaj", "E"],
];

fn camelot(s: &str) -> u8 {
    let k: String = s.trim().chars().take(12).collect();
    for (row, aliases) in CAMELOT.iter().enumerate() {
        if aliases.iter().any(|a| a.eq_ignore_ascii_case(&k)) {
            let n = (row / 2 + 1) as u8;
            return if row % 2 == 1 { 12 + n } else { n };
        }
    }
    0
}

/// mStream's lofty22_year: after whitespace, four ASCII digits.
fn year_of(tag: &Tag) -> Option<i64> {
    tag.get_string(ItemKey::Year).or_else(|| tag.get_string(ItemKey::RecordingDate)).and_then(|s| {
        let d: Vec<i64> = s.chars().skip_while(|c| c.is_whitespace()).take_while(char::is_ascii_digit).take(4).filter_map(|c| c.to_digit(10).map(i64::from)).collect();
        (d.len() == 4).then(|| d.iter().fold(0, |y, x| y * 10 + x))
    })
}

/// mStream's parse_num_of.
fn num_of(s: &str) -> (Option<u32>, Option<u32>) {
    let (n, t) = match s.split_once('/') {
        Some((n, t)) => (n, Some(t)),
        None => (s, None),
    };
    (n.trim().parse::<u32>().ok(), t.and_then(|t| t.trim().parse::<u32>().ok()))
}

fn texts(tag: &Tag, key: ItemKey) -> Vec<String> {
    tag.get_items(key).filter_map(|i| match i.value() {
        ItemValue::Text(s) => Some(s.to_string()),
        _ => None,
    }).collect()
}

fn id3v1_text(s: &str) -> Option<String> {
    let t = s.trim();
    (!t.is_empty()).then(|| t.to_string())
}

fn blank(v: &str) -> bool {
    v.split('\u{1f}').all(|x| x.trim().is_empty())
}

fn clamp(v: Option<u32>) -> u16 {
    v.map_or(0, |n| n.min(65535) as u16)
}

fn fnv(data: &[u8]) -> String {
    let mut h: u64 = 0xcbf29ce484222325;
    for b in data {
        h ^= *b as u64;
        h = h.wrapping_mul(0x100000001b3);
    }
    format!("{h:016x}")
}

// ---------------------------------------------------------------------------
// One file's record.
// ---------------------------------------------------------------------------
fn record(bytes: &[u8], ext: &str, anchors: &Value) -> Value {
    let container = match ext {
        "mp3" => 1,
        "flac" => 2,
        "opus" => 3,
        _ => 0,
    };
    let p = match read(bytes, ext) {
        Ok(p) => p,
        Err(e) => {
            return json!({"container": container, "flags": 1 << 6, "known": 0, "durationMs": 0, "error": e,
                "fields": vec![""; 12], "year": 0, "track": 0, "trackTotal": 0, "disc": 0, "discTotal": 0, "bpm10": 0,
                "rgTrackGain": 0, "rgAlbumGain": 0, "rgTrackPeak": 0, "rgAlbumPeak": 0, "camelot": 0,
                "picOffset": 0, "picLength": 0, "picType": 0, "picMime": 0, "picCoding": 0, "picFnv": ""});
        }
    };
    let duration = p.file.properties().duration().as_millis() as u64;
    let tag = p.file.primary_tag().or_else(|| p.file.first_tag());
    let ttype = tag.map(|t| t.tag_type());
    let mut trunc = false;
    let mut f: Vec<String> = vec![String::new(); 12];
    let (mut year, mut track, mut track_total, mut disc, mut disc_total) = (None, None, None, None, None);
    let (mut bpm, mut cam, mut comp) = (0u16, 0u8, 0u8);
    let (mut rg_t, mut rg_a, mut pk_t, mut pk_a) = (None, None, 0u16, 0u16);
    let mut from_r128 = false;
    let mut pics: Vec<Picture> = Vec::new();
    if let Some(t) = tag {
        let v1 = ttype == Some(TagType::Id3v1);
        let tx = |key| {
            let v = texts(t, key);
            if v1 { v.iter().filter_map(|s| id3v1_text(s)).collect() } else { v }
        };
        f[0] = single(&tx(ItemKey::TrackTitle), &mut trunc);
        f[1] = list(&tx(ItemKey::TrackArtist), &mut trunc);
        f[2] = single(&tx(ItemKey::AlbumTitle), &mut trunc);
        let aa: Vec<String> = if ttype == Some(TagType::Id3v2) {
            match &p.parts.tpe2 {
                Some(v) if !v.is_empty() => v.clone(),
                _ => p.parts.custom.iter().find(|(k, v)| (k == "ALBUMARTIST" || k == "ALBUM ARTIST") && !v.trim().is_empty()).map(|(_, v)| vec![v.clone()]).unwrap_or_default(),
            }
        } else {
            texts(t, ItemKey::AlbumArtist)
        };
        f[3] = list(&aa, &mut trunc);
        f[4] = list(&tx(ItemKey::Genre), &mut trunc);
        f[5] = list(&texts(t, ItemKey::Composer), &mut trunc);
        f[6] = single(&texts(t, ItemKey::TrackTitleSortOrder), &mut trunc);
        f[7] = single(&texts(t, ItemKey::TrackArtistSortOrder), &mut trunc);
        f[8] = single(&texts(t, ItemKey::AlbumTitleSortOrder), &mut trunc);
        f[9] = single(&texts(t, ItemKey::AlbumArtistSortOrder), &mut trunc);
        let trimmed = |key| texts(t, key).iter().map(|s| s.trim().to_string()).collect::<Vec<_>>();
        f[10] = single(&trimmed(ItemKey::MusicBrainzReleaseId), &mut trunc);
        f[11] = single(&trimmed(ItemKey::MusicBrainzRecordingId), &mut trunc);
        year = year_of(t);
        track = t.track();
        track_total = t.track_total();
        disc = t.disk();
        disc_total = t.disk_total();
        if track.is_none() || track_total.is_none() {
            if let Some(raw) = t.get_string(ItemKey::TrackNumber) {
                let (n, tot) = num_of(raw);
                track = track.or(n);
                track_total = track_total.or(tot);
            }
        }
        if disc.is_none() || disc_total.is_none() {
            if let Some(raw) = t.get_string(ItemKey::DiscNumber) {
                let (n, tot) = num_of(raw);
                disc = disc.or(n);
                disc_total = disc_total.or(tot);
            }
        }
        if let Some(s) = t.get_string(ItemKey::Bpm).or_else(|| t.get_string(ItemKey::IntegerBpm)) {
            bpm = bpm10(s);
        }
        if let Some(s) = t.get_string(ItemKey::InitialKey) {
            cam = camelot(s);
        }
        if let Some(s) = t.get_string(ItemKey::FlagCompilation) {
            comp = compilation(s);
        }
        if ext == "opus" {
            // Opus (RFC 7845, 5.2.1): the gains are R128's.
            rg_t = t.get_string(ItemKey::R128TrackGain).and_then(r128);
            rg_a = t.get_string(ItemKey::R128AlbumGain).and_then(r128);
            from_r128 = rg_t.is_some() || rg_a.is_some();
        } else {
            rg_t = t.get_string(ItemKey::ReplayGainTrackGain).and_then(gain);
            rg_a = t.get_string(ItemKey::ReplayGainAlbumGain).and_then(gain);
        }
        pk_t = t.get_string(ItemKey::ReplayGainTrackPeak).map_or(0, peak);
        pk_a = t.get_string(ItemKey::ReplayGainAlbumPeak).map_or(0, peak);
        pics = match ttype {
            Some(TagType::Id3v2) => p.parts.id3_pictures.clone(),
            Some(TagType::Ape) => p.parts.ape_pictures.clone(),
            Some(TagType::Id3v1) => Vec::new(),
            _ => t.pictures().to_vec(),
        };
        // 5.1: ID3v1 fills a blank title, artist, album or genre and an
        // absent year or track number of another chosen tag.
        if !v1 {
            if let Some(v1) = p.file.tag(TagType::Id3v1) {
                let fill = |f: &mut String, key: ItemKey| {
                    if blank(f) {
                        if let Some(v) = v1.get_string(key).and_then(id3v1_text) {
                            *f = single(&[v], &mut false);
                        }
                    }
                };
                fill(&mut f[0], ItemKey::TrackTitle);
                fill(&mut f[1], ItemKey::TrackArtist);
                fill(&mut f[2], ItemKey::AlbumTitle);
                fill(&mut f[4], ItemKey::Genre);
                if year.is_none() {
                    year = year_of(v1);
                }
                if track.is_none() {
                    track = v1.track();
                }
            }
        }
    }
    // The picture (5.3): every non-empty one; the first front cover, else
    // the first; its anchor from anchors.json.
    pics.retain(|p| !p.data().is_empty());
    let elected = pics.iter().find(|p| p.pic_type() == PictureType::CoverFront).or_else(|| pics.first());
    // When the corpus doesn't know the elected bytes (lofty read a picture
    // short, say), the record's anchor is unknown here: picFnv names the
    // image bytes the anchor must read back to.
    let (mut po, mut pl, mut pt, mut pm, mut pc) = (0u64, 0u64, 0u64, 0u64, 0u64);
    let mut pic_fnv = String::new();
    if let Some(pic) = elected {
        let h = fnv(pic.data());
        pt = pic.pic_type().as_u8() as u64;
        pm = match pic.mime_type() {
            Some(MimeType::Jpeg) => 1,
            Some(MimeType::Png) => 2,
            _ => 3,
        };
        if let Some(a) = anchors["pictures"].as_array().and_then(|v| v.iter().find(|a| a["fnv"] == h.as_str())) {
            po = a["offset"].as_u64().unwrap();
            pl = a["length"].as_u64().unwrap();
            pc = a["coding"].as_u64().unwrap();
        }
        pic_fnv = h;
    }
    let mut flags: u64 = comp as u64;
    if rg_t.is_some() {
        flags |= 1 << 2;
    }
    if rg_a.is_some() {
        flags |= 1 << 3;
    }
    if from_r128 {
        flags |= 1 << 4;
    }
    if trunc {
        flags |= 1 << 7;
    }
    let numbers = [clamp(year.map(|y| y as u32)), clamp(track), clamp(track_total), clamp(disc), clamp(disc_total)];
    let any = f.iter().any(|s| !s.is_empty()) || numbers.iter().any(|n| *n != 0) || bpm != 0 || cam != 0 || comp != 0
        || rg_t.is_some() || rg_a.is_some() || pk_t != 0 || pk_a != 0 || !pic_fnv.is_empty();
    if !any {
        flags |= 1 << 5;
    }
    json!({
        "container": container, "flags": flags, "known": 0x1FFFF, "durationMs": duration, "fields": f,
        "year": numbers[0], "track": numbers[1], "trackTotal": numbers[2], "disc": numbers[3], "discTotal": numbers[4],
        "bpm10": bpm, "camelot": cam, "rgTrackGain": rg_t.unwrap_or(0), "rgAlbumGain": rg_a.unwrap_or(0),
        "rgTrackPeak": pk_t, "rgAlbumPeak": pk_a,
        "picOffset": po, "picLength": pl, "picType": pt, "picMime": pm, "picCoding": pc, "picFnv": pic_fnv,
    })
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let check = args.iter().any(|a| a == "--check");
    let dir: PathBuf = args.iter().position(|a| a == "--dir").and_then(|i| args.get(i + 1)).map(PathBuf::from).unwrap_or_else(|| Path::new("test/fixtures/tags").to_path_buf());
    let anchors: Value = serde_json::from_str(&std::fs::read_to_string(dir.join("anchors.json")).expect("anchors.json")).expect("anchors.json is JSON");
    let mut out = BTreeMap::new();
    for (name, a) in anchors.as_object().expect("an object") {
        let bytes = std::fs::read(dir.join(name)).expect("a corpus file");
        let ext = name.rsplit('.').next().unwrap_or("");
        let mut r = record(&bytes, ext, a);
        r["note"] = a["note"].clone();
        out.insert(name.clone(), r);
    }
    let text = serde_json::to_string_pretty(&out).unwrap() + "\n";
    let path = dir.join("expected.json");
    if check {
        let old = std::fs::read_to_string(&path).unwrap_or_default();
        if old != text {
            eprintln!("differs: {}", path.display());
            std::process::exit(1);
        }
        println!("expected.json: {} records match", out.len());
    } else {
        std::fs::write(&path, text).expect("write expected.json");
        println!("wrote {} records to {}", out.len(), path.display());
    }
}
