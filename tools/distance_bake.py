#!/usr/bin/env python3
"""
distance_bake.py -- bake distance variants of soundmd.ini sounds for CosmeticExt.

Why bake
--------
gamemd's mixer gives each playing sound a volume and a pan and nothing else:
no filter stage, no reverb send. So the frequency side of hearing something
far away -- the high end soaked up by the air, the ground reflection combing
the spectrum, the scattered tail that smears it -- cannot be applied while the
game runs. This tool applies it ahead of time, the way a sound designer would
in an editor, but from the physics:

  * For every sound marked `Distance.Bake=yes` it renders one variant per
    distance band from that sound's own samples.
  * The variants go into an audioNN.bag/.idx pair. Antares (and Ares) load
    audio01..audio99 at startup on top of audio.bag, so no game file is
    replaced.
  * It writes a copy of soundmd.ini in which each variant is a sound of its own
    and the base lists them in Distance.Bands / Distance.Bands.From.
    CosmeticExt.dll then picks the band from the real distance at play time.

What is NOT baked: spreading loss (-6 dB per doubling). CosmeticExt applies it
continuously at run time (Distance.Falloff=inverse, which this tool switches on
for baked sounds that do not set a falloff), so baking it too would count it
twice.

The physics, per band
---------------------
A band's representative distance d is midway to the next band (1.5x + 2
past the start for the last one), in cells beyond the screen edge. With
Distance.Reference = ref (the on-screen distance, also the DLL's spreading
reference) and CellMeters = m, the source is (ref + d) * m metres away, of
which d * m is path the on-screen sound does not already have.

1. Air absorption -- ISO 9613-1 pure-tone atmospheric absorption
   alpha(f, T, RH, p) in dB/m over the extra path d * m:
   |H(f)| = 10^(-alpha * d * m * Air / 20). At 20 C / 70 % RH that is
   ~5 dB/km at 1 kHz but ~77 dB/km at 8 kHz: distance takes the top off first.
2. Ground reflection -- a second path via the source's mirror image in the
   ground at the full distance r = (ref + d) * m, path difference
   sqrt(r^2+(hs+hr)^2) - sqrt(r^2+(hs-hr)^2), reflection coefficient Ground:
   H = (1 + g e^{-j 2 pi f tau}) / sqrt(1+g^2) -- the comb filter that makes
   distant sound hollow (first notch at 1/(2 tau)). Energy-normalised, so it
   reshapes the spectrum without changing the level.
3. Scattering tail -- the result convolved with exponentially decaying noise
   (pre-delay and T60 growing with band), itself air-absorbed over a 1.5x
   longer extra path, mixed at Tail scaled by band: the "rolling" of far
   thunder.

Filters 1+2 are one linear-phase FIR (frequency-sampled, Blackman window) with
its delay removed, so the variant starts exactly when the original does.

Variants are written as 16-bit PCM by default. IMA ADPCM (--format adpcm, the
codec of the game's own audio.bag, 1/4 the size) adds white quantisation noise
about 20 dB under the signal; on a full-range sample that is masked, but on a
far band it puts the removed top octave back as hiss: measured on gexp14a, the
6.4-11 kHz band of band 3 goes from -56 dB (PCM) to -20 dB (ADPCM).

Usage
-----
    distance_bake.py --soundmd SOUNDMD.INI --game RA2_DIR --out OUT_DIR
                     [--bag 97] [--format pcm|adpcm] [--only A,B]
                     [--search DIR ...] [--preview WAV_DIR] [--dry-run]

Reads samples from loose files in --game / --search dirs and from every MIX in
--game (nested archives included), in the engine's override order. Writes
OUT_DIR/audioNN.idx, audioNN.bag, soundmd.ini and bake-report.txt. The input
soundmd.ini is never modified; re-running on an already-baked file first
strips what a previous run generated (lines and sections marked ;cebake).

Tags (in a sound's section; any Distance.Bake.* key in [CosmeticExt] is the
default for every baked sound) -- see docs/soundmd-tags.md:

    Distance.Bake=yes
    Distance.Bake.Bands=4,14,32     band starts, cells beyond the screen edge
    Distance.Bake.CellMeters=25     world scale for the physics
    Distance.Bake.Temperature=20    deg C
    Distance.Bake.Humidity=70       % relative humidity
    Distance.Bake.Air=1             absorption multiplier (artistic scale)
    Distance.Bake.Ground=0.5        ground reflection coefficient, 0 = off
    Distance.Bake.SourceHeight=2    m
    Distance.Bake.ListenerHeight=30 m (the camera)
    Distance.Bake.Tail=0.35         scattering-tail level at the farthest band
    Distance.Bake.TailTime=1.4      T60 seconds at the farthest band
    Distance.Bake.Range=auto        Range= of the variants (auto: 2*last+10)
"""

import argparse
import hashlib
import math
import os
import re
import struct
import sys
import wave

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mixread  # noqa: E402

MARK = ';cebake'

# ---------------------------------------------------------------------------
# Physics
# ---------------------------------------------------------------------------


def air_absorption(f, temp_c=20.0, rh=70.0, pa_kpa=101.325):
    """ISO 9613-1:1993 eq. (3)-(5): pure-tone absorption in dB per metre."""
    f = np.asarray(f, dtype=float)
    T = temp_c + 273.15
    T0 = 293.15     # reference temperature
    T01 = 273.16    # triple-point isotherm
    pr = 101.325    # reference pressure, kPa
    psat_pr = 10.0 ** (-6.8346 * (T01 / T) ** 1.261 + 4.6151)
    h = rh * psat_pr * (pr / pa_kpa)  # molar concentration of water vapour, %
    frO = (pa_kpa / pr) * (24.0 + 4.04e4 * h * (0.02 + h) / (0.391 + h))
    frN = (pa_kpa / pr) * (T / T0) ** -0.5 * (
        9.0 + 280.0 * h * math.exp(-4.170 * ((T / T0) ** (-1.0 / 3.0) - 1.0)))
    f2 = f * f
    return 8.686 * f2 * (
        1.84e-11 * (pr / pa_kpa) * (T / T0) ** 0.5
        + (T / T0) ** -2.5 * (
            0.01275 * math.exp(-2239.1 / T) / (frO + f2 / frO)
            + 0.1068 * math.exp(-3352.0 / T) / (frN + f2 / frN)))


def speed_of_sound(temp_c):
    return 331.3 * math.sqrt(1.0 + temp_c / 273.15)


def ground_delay(r, hs, hr, c):
    """Extra travel time of the ground-reflected path, seconds."""
    direct = math.hypot(r, hs - hr)
    mirror = math.hypot(r, hs + hr)
    return (mirror - direct) / c


def response(freqs, r_air, r_geom, cfg, ground=True):
    """Complex response of the air absorption over r_air metres (the path
    beyond the on-screen distance: the original sample already IS the
    on-screen sound) and, optionally, of the ground reflection for a source
    r_geom metres away (the full distance: the close-recorded original has no
    reflection in it)."""
    H = 10.0 ** (-air_absorption(freqs, cfg['temperature'], cfg['humidity'])
                 * r_air * cfg['air'] / 20.0)
    H = H.astype(complex)
    g = cfg['ground']
    if ground and g > 0.0:
        tau = ground_delay(r_geom, cfg['sourceheight'], cfg['listenerheight'],
                           speed_of_sound(cfg['temperature']))
        H *= (1.0 + g * np.exp(-2j * np.pi * freqs * tau)) / math.sqrt(1.0 + g * g)
    return H


def design_fir(fs, r_air, r_geom, cfg, ground=True):
    """Linear-phase FIR for response(); returns (taps, delay_in_samples)."""
    tau_s = 0.0
    if ground and cfg['ground'] > 0.0:
        tau_s = ground_delay(r_geom, cfg['sourceheight'], cfg['listenerheight'],
                             speed_of_sound(cfg['temperature'])) * fs
    ntaps = 1023
    while ntaps // 2 < tau_s + 64:
        ntaps = ntaps * 2 + 1
    nfft = 1
    while nfft < 8 * ntaps:
        nfft *= 2
    freqs = np.fft.rfftfreq(nfft, 1.0 / fs)
    h = np.fft.irfft(response(freqs, r_air, r_geom, cfg, ground), nfft)
    half = ntaps // 2
    h = np.concatenate((h[-half:], h[:half + 1]))  # centre the zero-phase part
    h *= np.blackman(ntaps)
    return h, half


def fft_convolve(a, b):
    n = len(a) + len(b) - 1
    nfft = 1
    while nfft < n:
        nfft *= 2
    return np.fft.irfft(np.fft.rfft(a, nfft) * np.fft.rfft(b, nfft), nfft)[:n]


def stable_seed(*parts):
    return int.from_bytes(hashlib.sha1('|'.join(parts).encode()).digest()[:8], 'little')


def bake_signal(x, fs, r_air, r_geom, band, nbands, cfg, seed_key):
    """x: float array (frames, channels) in [-1, 1]. Returns (variant, gain)
    where gain < 1 is how far the clip guard had to turn it down."""
    h, delay = design_fir(fs, r_air, r_geom, cfg)
    channels = []
    for ch in range(x.shape[1]):
        y = fft_convolve(x[:, ch], h)[delay:delay + x.shape[0]]
        channels.append(y)
    y = np.stack(channels, axis=1)

    weight = (band + 1) / nbands
    level = cfg['tail'] * weight
    if level > 0.0 and cfg['tailtime'] > 0.0:
        t60 = max(0.05, cfg['tailtime'] * weight)
        pre = int((0.012 + 0.010 * band) * fs)  # first scattered arrivals
        n = int(t60 * fs)
        rng = np.random.default_rng(stable_seed(seed_key, str(band)))
        t = np.arange(n) / fs
        ir = rng.standard_normal(n) * np.exp(-6.9078 * t / t60)
        ramp = min(n, int(0.004 * fs))
        ir[:ramp] *= np.linspace(0.0, 1.0, ramp, endpoint=False)
        hs, hd = design_fir(fs, r_air * 1.5, r_geom, cfg, ground=False)
        ir = fft_convolve(ir, hs)[hd:hd + n]
        ir = np.concatenate((np.zeros(pre), ir))
        ir /= math.sqrt(float(np.sum(ir * ir))) or 1.0
        wet = np.stack([fft_convolve(y[:, ch], ir) for ch in range(y.shape[1])], axis=1)
        out = np.zeros_like(wet)
        out[:y.shape[0]] += y
        y = out + level * wet

    # Trim the silent end of the tail, then a 5 ms fade so nothing clicks.
    env = np.max(np.abs(y), axis=1)
    peak = float(env.max()) if env.size else 0.0
    gain = 1.0
    if peak > 0.0:
        above = np.nonzero(env > peak * 10 ** (-60 / 20))[0]
        end = max(int(above[-1]) + 1, x.shape[0]) if above.size else x.shape[0]
        y = y[:end]
        fade = min(len(y), int(0.005 * fs))
        if fade > 0:
            y[-fade:] *= np.linspace(1.0, 0.0, fade)[:, None]
        if peak > 0.98:
            gain = 0.98 / peak
            y *= gain
    return y, gain


# ---------------------------------------------------------------------------
# Westwood audio.bag / audio.idx
# ---------------------------------------------------------------------------

IMA_STEP = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767]
IMA_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8] * 2

FLAG_STEREO = 1
FLAG_PCM = 2
FLAG_16BIT = 4
FLAG_ADPCM = 8
IDX_MAGIC = b'GABA'
NAME_MAX = 15  # AudioIDXEntry::Name is char[16]


def _ima_step(nib, pred, idx):
    step = IMA_STEP[idx]
    diff = step >> 3
    if nib & 4:
        diff += step
    if nib & 2:
        diff += step >> 1
    if nib & 1:
        diff += step >> 2
    pred = pred - diff if nib & 8 else pred + diff
    pred = -32768 if pred < -32768 else 32767 if pred > 32767 else pred
    idx += IMA_INDEX[nib]
    idx = 0 if idx < 0 else 88 if idx > 88 else idx
    return pred, idx


def ima_decode(data, chunk, channels):
    """Westwood bag IMA ADPCM. Mono: 4-byte header (int16 sample, u8 index,
    pad) + nibbles, low nibble first -- verified against gamemd's audio.bag
    (block boundaries are seamless only in this order). Stereo: one header per
    channel, then bytes alternating L, R. Measured on the five stereo ADPCM
    samples in gamemd's audio.bag: this decodes 2-3x smoother than the MS-IMA
    4-byte-group layout or a nibble-per-channel split."""
    out = [[] for _ in range(channels)]
    for b0 in range(0, len(data), chunk):
        blk = data[b0:b0 + chunk]
        if len(blk) < 4 * channels:
            break
        preds, idxs = [], []
        for ch in range(channels):
            p, i = struct.unpack_from('<hB', blk, 4 * ch)
            preds.append(p)
            idxs.append(min(88, i))
            out[ch].append(p)
        body = blk[4 * channels:]
        if channels == 1:
            p, i = preds[0], idxs[0]
            o = out[0]
            for byte in body:
                p, i = _ima_step(byte & 15, p, i)
                o.append(p)
                p, i = _ima_step(byte >> 4, p, i)
                o.append(p)
        else:
            for k, byte in enumerate(body):
                ch = k % channels
                p, i = _ima_step(byte & 15, preds[ch], idxs[ch])
                out[ch].append(p)
                p, i = _ima_step(byte >> 4, p, i)
                out[ch].append(p)
                preds[ch], idxs[ch] = p, i
    n = min(len(c) for c in out)
    return np.array([c[:n] for c in out], dtype=np.int16).T


def _ima_nibbles(samples, pred, idx):
    """Encode `samples` from decoder state (pred, idx); returns nibbles and
    the state after them, tracking the decoder exactly so the two never drift."""
    nibs = []
    for v in samples:
        diff = int(v) - pred
        nib = 0
        if diff < 0:
            nib = 8
            diff = -diff
        step = IMA_STEP[idx]
        if diff >= step:
            nib |= 4
            diff -= step
        step >>= 1
        if diff >= step:
            nib |= 2
            diff -= step
        step >>= 1
        if diff >= step:
            nib |= 1
        pred, idx = _ima_step(nib, pred, idx)
        nibs.append(nib)
    return nibs, idx


def ima_encode(pcm):
    """Inverse of ima_decode. pcm: int16 array (frames, channels), 1 or 2
    channels. Returns (bytes, chunk) in the layout gamemd's own bag uses:
    512-byte blocks per channel. Pads the last block with silence."""
    pcm = np.asarray(pcm, dtype=np.int32)
    channels = pcm.shape[1]
    chunk = 512 * channels
    per_block = 1 + (512 - 4) * 2
    nblocks = max(1, -(-pcm.shape[0] // per_block))
    pad = np.zeros((nblocks * per_block - pcm.shape[0], channels), dtype=np.int32)
    pcm = np.concatenate((pcm, pad))
    out = bytearray()
    idxs = [0] * channels
    for b in range(nblocks):
        s = pcm[b * per_block:(b + 1) * per_block]
        streams = []
        for ch in range(channels):
            pred = int(s[0, ch])
            out += struct.pack('<hBB', pred, idxs[ch], 0)
            nibs, idxs[ch] = _ima_nibbles(s[1:, ch], pred, idxs[ch])
            streams.append([nibs[k] | (nibs[k + 1] << 4) for k in range(0, len(nibs), 2)])
        for k in range(len(streams[0])):
            for ch in range(channels):
                out.append(streams[ch][k])
    return bytes(out), chunk


def read_idx(blob):
    if blob[:4] != IDX_MAGIC:
        raise ValueError('not a GABA audio index')
    _, version, count = struct.unpack_from('<4sII', blob, 0)
    size = 36 if version != 1 else 32
    entries = []
    for k in range(count):
        if version == 1:
            name, off, sz, rate, flags = struct.unpack_from('<16siiII', blob, 12 + k * size)
            chunk = 0
        else:
            name, off, sz, rate, flags, chunk = struct.unpack_from('<16siiIII', blob, 12 + k * size)
        entries.append((name.split(b'\0', 1)[0].decode('latin1'), off, sz, rate, flags, chunk))
    return entries


def write_bag(samples, idx_path, bag_path):
    """samples: list of (name, rate, flags, chunk, bytes). Entries sorted the
    way the engine's binary search (and Antares' merge) expects: _strcmpi."""
    samples = sorted(samples, key=lambda s: s[0].lower())
    body = bytearray()
    index = bytearray(struct.pack('<4sII', IDX_MAGIC, 2, len(samples)))
    for name, rate, flags, chunk, data in samples:
        raw = name.encode('latin1')
        if len(raw) > NAME_MAX:
            raise ValueError(f'sample name too long: {name}')
        index += struct.pack('<16siiIII', raw, len(body), len(data), rate, flags, chunk)
        body += data
    with open(idx_path, 'wb') as f:
        f.write(index)
    with open(bag_path, 'wb') as f:
        f.write(body)


def decode_entry(data, flags, chunk):
    channels = 2 if flags & FLAG_STEREO else 1
    if flags & FLAG_ADPCM:
        return ima_decode(data, chunk, channels)
    if flags & FLAG_16BIT:
        return np.frombuffer(data[:len(data) // (2 * channels) * 2 * channels],
                             dtype='<i2').reshape(-1, channels).copy()
    a = np.frombuffer(data, dtype=np.uint8)
    a = a[:len(a) // channels * channels].astype(np.int16)
    return ((a - 128) << 8).reshape(-1, channels)


def read_wav(blob):
    """PCM 8/16-bit WAV (what Antares loose files are in practice)."""
    if blob[:4] != b'RIFF' or blob[8:12] != b'WAVE':
        raise ValueError('not a RIFF/WAVE file')
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(blob):
        cid, size = struct.unpack_from('<4sI', blob, pos)
        payload = blob[pos + 8:pos + 8 + size]
        if cid == b'fmt ':
            fmt = struct.unpack_from('<HHIIHH', payload, 0)
        elif cid == b'data':
            data = payload
        pos += 8 + size + (size & 1)
    if not fmt or data is None:
        raise ValueError('WAV without fmt/data')
    tag, channels, rate, _, align, bits = fmt
    if tag == 0x11:
        flags = FLAG_ADPCM | FLAG_16BIT | (FLAG_STEREO if channels == 2 else 0)
        return decode_entry(data, flags, align), rate
    if tag != 1 or bits not in (8, 16):
        raise ValueError(f'unsupported WAV format tag {tag} / {bits}-bit')
    flags = (FLAG_16BIT if bits == 16 else 0) | (FLAG_STEREO if channels == 2 else 0)
    return decode_entry(data, flags, 0), rate


def write_wav(path, pcm, rate):
    with wave.open(path, 'wb') as w:
        w.setnchannels(pcm.shape[1])
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm.astype('<i2').tobytes())


# ---------------------------------------------------------------------------
# Finding samples the way the game does
# ---------------------------------------------------------------------------


def mix_priority(path):
    """Engine search order: expansion mixes first (highest number wins), then
    the YR mixes, then RA2's, then anything else."""
    n = os.path.basename(path).lower()
    m = re.match(r'expandmd(\d+)\.mix$', n)
    if m:
        return (0, -int(m.group(1)), n)
    order = ['ra2md.mix', 'langmd.mix', 'ra2.mix', 'language.mix']
    if n in order:
        return (1, order.index(n), n)
    return (2, 0, n)


OUR_NAME = re.compile(r'(_d\d+|^ce[0-9a-f]{9}_\d+)$', re.I)


def written_by_us(idx_blob):
    """True when every sample in an audioNN.idx has a name this tool makes."""
    try:
        return all(OUR_NAME.search(e[0]) for e in read_idx(idx_blob))
    except (ValueError, struct.error):
        return False


class SampleBank:
    def __init__(self, game_dir, search_dirs, skip_bag):
        self.dirs = [d for d in [game_dir] + list(search_dirs) if d]
        self.loose = {}     # lower file name -> path; first directory wins
        for d in self.dirs:
            for f in sorted(os.listdir(d)):
                self.loose.setdefault(f.lower(), os.path.join(d, f))
        # Every MIX (nested archives included) is walked once; the first copy
        # of a file id in search order wins, as in the engine.
        self.mix_index = {}  # file id -> (label, MixFile)
        mixes = []
        if game_dir:
            mixes = sorted((os.path.join(game_dir, f) for f in os.listdir(game_dir)
                            if f.lower().endswith('.mix')), key=mix_priority)
        labels = mixread.name_table()
        for path in mixes:
            fh = open(path, 'rb')
            try:
                root = mixread.MixFile(fh, name=os.path.basename(path))
            except mixread.MixError:
                fh.close()
                continue
            for chain, m in mixread.walk(root, labels):
                for eid in m.entries:
                    self.mix_index.setdefault(eid, (os.path.basename(path) + chain, m))
        self.entries = {}   # lower name -> (name, bag_key, off, size, rate, flags, chunk)
        self.bags = {}      # bag_key -> bytes
        self.sources = []   # human-readable list of loaded bags
        # Antares' Audio_Load order; later bags override earlier ones.
        bases = ['audio', 'ares'] + [f'audio{k:02d}' for k in range(1, 100)]
        for base in bases:
            if base == skip_bag:
                continue
            idx = self.find_file(base + '.idx')
            if idx is None:
                continue
            bag = self.find_file(base + '.bag')
            if bag is None:
                print(f'warning: {base}.idx without {base}.bag -- ignored')
                continue
            self.bags[base] = bag[1]
            n = 0
            for name, off, size, rate, flags, chunk in read_idx(idx[1]):
                self.entries[name.lower()] = (name, base, off, size, rate, flags, chunk)
                n += 1
            self.sources.append(f'{base}.bag ({n} samples, from {idx[0]})')

    def find_file(self, name):
        """(where, bytes) of the copy the game would open, or None."""
        p = self.loose.get(name.lower())
        if p:
            with open(p, 'rb') as fh:
                return p, fh.read()
        hit = self.mix_index.get(mixread.mix_id(name))
        if hit:
            label, m = hit
            return label, m.read(mixread.mix_id(name))
        return None

    def has(self, name):
        return name.lower() in self.entries

    def load(self, name):
        """(int16 array (frames, ch), rate, where) or raises KeyError."""
        e = self.entries.get(name.lower())
        if e:
            _, bag, off, size, rate, flags, chunk = e
            return decode_entry(self.bags[bag][off:off + size], flags, chunk), rate, f'{bag}.bag'
        hit = self.find_file(name + '.wav')  # Antares loose sample
        if hit:
            pcm, rate = read_wav(hit[1])
            return pcm, rate, hit[0]
        raise KeyError(name)


# ---------------------------------------------------------------------------
# soundmd.ini, edited line-by-line so everything else stays byte-identical
# ---------------------------------------------------------------------------

_HEADER = re.compile(r'^\s*\[([^\]]+)\]')


class Ini:
    def __init__(self, text):
        self.lines = text.splitlines()
        self.reindex()

    def reindex(self):
        heads = [(i, _HEADER.match(l).group(1).strip())
                 for i, l in enumerate(self.lines) if _HEADER.match(l)]
        self.blocks = []  # (name, start, end) in file order, duplicates included
        for k, (i, name) in enumerate(heads):
            end = heads[k + 1][0] if k + 1 < len(heads) else len(self.lines)
            self.blocks.append((name, i, end))
        self.first = {}
        for b in self.blocks:
            self.first.setdefault(b[0].lower(), b)  # reads and edits use the first occurrence

    def has(self, section):
        return section.lower() in self.first

    def items(self, section):
        b = self.first.get(section.lower())
        if not b:
            return []
        out = []
        for i in range(b[1] + 1, b[2]):
            line = self.lines[i].split(';', 1)[0]
            if '=' in line:
                k, v = line.split('=', 1)
                if k.strip():
                    out.append((k.strip(), v.strip(), i))
        return out

    def get(self, section, key, default=None):
        for k, v, _ in self.items(section):
            if k.lower() == key.lower():
                return v
        return default

    def insert(self, section, new_lines):
        b = self.first[section.lower()]
        at = b[1] + 1
        for i in range(b[1] + 1, b[2]):
            if self.lines[i].strip() and not self.lines[i].lstrip().startswith(';'):
                at = i + 1
        self.lines[at:at] = new_lines
        self.reindex()

    def append_section(self, name, body):
        while self.lines and not self.lines[-1].strip():
            self.lines.pop()
        if self.lines and self.lines[-1].startswith('; EOF guard'):
            self.lines.pop()
        self.lines += ['', f'[{name}]'] + body
        self.reindex()

    def strip_generated(self):
        """Drop everything a previous bake added (idempotent re-runs)."""
        keep, skip = [], False
        for i, line in enumerate(self.lines):
            if _HEADER.match(line):
                nxt = self.lines[i + 1] if i + 1 < len(self.lines) else ''
                skip = nxt.strip().startswith(MARK)
                if skip:
                    while keep and not keep[-1].strip():
                        keep.pop()
                    continue
            if skip or MARK in line:
                continue
            keep.append(line)
        removed = len(self.lines) - len(keep)
        self.lines = keep
        self.reindex()
        return removed

    def text(self):
        lines = list(self.lines)
        while lines and not lines[-1].strip():
            lines.pop()
        if not (lines and lines[-1].startswith('; EOF guard')):
            # YR ignores an INI's last line; keep a throwaway one there.
            lines += ['', '; EOF guard -- the engine does not read the last line of an INI']
        return '\r\n'.join(lines) + '\r\n'


# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

BAKE_DEFAULTS = {
    'bands': [4.0, 14.0, 32.0],
    'cellmeters': 25.0,
    'temperature': 20.0,
    'humidity': 70.0,
    'air': 1.0,
    'ground': 0.5,
    'sourceheight': 2.0,
    'listenerheight': 30.0,
    'tail': 0.35,
    'tailtime': 1.4,
    'range': 'auto',
}
MAX_BANDS = 9  # keeps hashed variant names inside the 15-character limit
SCALAR_KEYS = ['cellmeters', 'temperature', 'humidity', 'air', 'ground',
               'sourceheight', 'listenerheight', 'tail', 'tailtime']


def truthy(v):
    return bool(v) and v.strip()[:1].lower() in ('y', 't', '1')


def parse_list(v):
    return [t for t in re.split(r'[\s,]+', v or '') if t]


def read_bake_cfg(ini, section, base, problems):
    cfg = dict(base)
    v = ini.get(section, 'Distance.Bake.Bands')
    if v is not None:
        try:
            bands = sorted(float(t) for t in parse_list(v))
            if not bands or bands[0] < 0:
                raise ValueError
            if len(bands) > MAX_BANDS:
                problems.append(f'[{section}] Distance.Bake.Bands lists {len(bands)} bands; '
                                f'only the first {MAX_BANDS} are baked')
                bands = bands[:MAX_BANDS]
            cfg['bands'] = bands
        except ValueError:
            problems.append(f'[{section}] Distance.Bake.Bands={v} is not a list of distances')
    for k in SCALAR_KEYS:
        v = ini.get(section, 'Distance.Bake.' + k)
        if v is None:
            # INI keys are case-insensitive; .get already handles that.
            continue
        try:
            cfg[k] = float(v)
        except ValueError:
            problems.append(f'[{section}] Distance.Bake.{k}={v} is not a number')
    v = ini.get(section, 'Distance.Bake.Range')
    if v is not None:
        cfg['range'] = v.strip().lower()
    cfg['ground'] = min(max(cfg['ground'], 0.0), 0.95)
    cfg['tail'] = max(cfg['tail'], 0.0)
    cfg['air'] = max(cfg['air'], 0.0)
    return cfg


def representative(bands):
    """A band's distance for the physics: midway to the next band, or 1.5x
    (+2) past the start for the last, open-ended one."""
    reps = []
    for i, f in enumerate(bands):
        reps.append((f + bands[i + 1]) / 2.0 if i + 1 < len(bands) else f * 1.5 + 2.0)
    return reps


def variant_sample_name(sample, band, render_key, taken):
    """<sample>_d<band> when it fits in 15 characters and is free, else a
    hashed ce<9 hex>_<band> that is never already taken."""
    name = f'{sample}_d{band}'
    if len(name) <= NAME_MAX and name.lower() not in taken:
        return name
    for salt in range(10000):
        h = hashlib.sha1(f'{sample.lower()}|{render_key}|{salt}'.encode()).hexdigest()[:9]
        name = f'ce{h}_{band}'
        if name.lower() not in taken:
            return name
    raise RuntimeError(f'no free sample name for {sample} band {band}')


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--soundmd', required=True)
    ap.add_argument('--game', help='RA2/YR directory (loose files and MIXes)')
    ap.add_argument('--search', action='append', default=[], help='extra loose-file directory')
    ap.add_argument('--out', required=True)
    ap.add_argument('--bag', type=int, default=97, help='write audioNN.bag/.idx (default 97)')
    ap.add_argument('--format', choices=('pcm', 'adpcm'), default='pcm',
                    help='pcm (default): 16-bit, 4x the size; adpcm: the codec of the game\'s own bag, '
                         'whose ~20 dB-SNR white noise refills the top octave the far bands remove')
    ap.add_argument('--only', help='comma list of sound names to bake')
    ap.add_argument('--preview', help='also write every original and variant as .wav here')
    ap.add_argument('--dry-run', action='store_true', help='report only, write nothing')
    ap.add_argument('--force-bag', action='store_true',
                    help='overwrite an existing audioNN that this tool did not write')
    a = ap.parse_args(argv)

    if not 1 <= a.bag <= 99:
        ap.error('--bag must be 1..99')
    bag_base = f'audio{a.bag:02d}'

    with open(a.soundmd, 'rb') as f:
        ini = Ini(f.read().decode('latin1'))
    stripped = ini.strip_generated()

    report = [f'distance_bake.py -- {a.soundmd}', '']
    if stripped:
        report.append(f'stripped {stripped} line(s) generated by a previous bake')

    bank = SampleBank(a.game, a.search, skip_bag=bag_base)
    report += ['sample sources:'] + [f'  {s}' for s in bank.sources] + ['']
    existing = bank.find_file(bag_base + '.idx')
    if existing and not a.force_bag and not written_by_us(existing[1]):
        print(f'error: {bag_base}.idx already exists ({existing[0]}) and was not written by '
              f'this tool -- pick another --bag or pass --force-bag', file=sys.stderr)
        return 2

    problems = []
    global_cfg = read_bake_cfg(ini, 'CosmeticExt', BAKE_DEFAULTS, problems)
    default_range = int(float(ini.get('Defaults', 'Range', '10') or 10))
    global_falloff = ini.get('CosmeticExt', 'Distance.Falloff')

    names = [v for _, v, _ in ini.items('SoundList')]
    only = {n.lower() for n in parse_list(a.only)} if a.only else None
    targets = [n for n in names if ini.has(n) and truthy(ini.get(n, 'Distance.Bake', ''))
               and (only is None or n.lower() in only)]
    if only:
        missing = only - {t.lower() for t in targets}
        for m in sorted(missing):
            problems.append(f'--only {m}: not in [SoundList] or has no Distance.Bake=yes')

    out_samples = []
    renders = {}  # render key -> sample name already in out_samples
    taken = set(bank.entries)
    soundlist_adds = []
    preview = a.preview if (a.preview and not a.dry_run) else None
    if preview:
        os.makedirs(preview, exist_ok=True)

    for sound in targets:
        cfg = read_bake_cfg(ini, sound, global_cfg, problems)
        bands = cfg['bands']
        reps = representative(bands)
        try:
            ref = float(ini.get(sound, 'Distance.Reference') or ini.get('CosmeticExt', 'Distance.Reference') or 8.0)
        except ValueError:
            problems.append(f'[{sound}] Distance.Reference is not a number -- using 8')
            ref = 8.0
        base_range = int(float(ini.get(sound, 'Range') or default_range))
        if cfg['range'] in ('auto', ''):
            var_range = max(base_range, int(math.ceil(bands[-1] * 2 + 10)))
        else:
            var_range = int(float(cfg['range']))

        tokens = parse_list(ini.get(sound, 'Sounds', ''))
        if not tokens:
            problems.append(f'[{sound}] has no Sounds= -- skipped')
            continue
        variants = [f'{sound}_D{i + 1}' for i in range(len(bands))]
        clash = [v for v in variants if ini.has(v)]
        if clash:
            problems.append(f'[{sound}] variant section {clash[0]} already exists and is not ours -- skipped')
            continue

        loaded = []
        for tok in tokens:
            m = re.match(r'^([$#]*)(.+)$', tok)
            prefix, sample = m.group(1), m.group(2)
            try:
                pcm, rate, where = bank.load(sample)
            except KeyError:
                problems.append(f'[{sound}] sample {sample} not found in any bag or as {sample}.wav -- sound skipped')
                loaded = None
                break
            loaded.append((prefix, sample, pcm, rate, where))
        if not loaded:
            continue

        report.append(f'[{sound}] {len(loaded)} sample(s), base Range={base_range}, variants Range={var_range}, '
                      f'{cfg["cellmeters"]:g} m/cell, {cfg["temperature"]:g} C, {cfg["humidity"]:g} % RH')
        if bands[0] >= 0.75 * base_range:
            problems.append(f'[{sound}] first band starts at {bands[0]:g} cells but the sound itself fades out from '
                            f'{0.75 * base_range:g} (Range={base_range}): lower Distance.Bake.Bands or raise Range')

        variant_tokens = [[] for _ in bands]
        for prefix, sample, pcm, rate, where in loaded:
            x = pcm.astype(float) / 32768.0
            if preview:
                write_wav(os.path.join(preview, f'{sound}__{sample}__0.wav'), pcm, rate)
            for i, rep in enumerate(reps):
                r_air, r_geom = rep * cfg['cellmeters'], (ref + rep) * cfg['cellmeters']
                # Sounds that list one sample twice, or share samples under
                # the same physics, reuse one render instead of baking twice.
                key = (sample.lower(), rate, i, len(bands), round(r_air, 6), round(r_geom, 6),
                       tuple(cfg[k] for k in SCALAR_KEYS), a.format)
                if key in renders:
                    variant_tokens[i].append(prefix + renders[key])
                    continue
                y, gain = bake_signal(x, rate, r_air, r_geom, i, len(bands), cfg, sample.lower())
                if gain < 0.89:  # beyond 1 dB the level step at the band edge is audible
                    report.append(f'  note: {sample} band {i + 1} turned down {-20 * math.log10(gain):.1f} dB '
                                  f'to stay below full scale')
                y16 = np.clip(np.round(y * 32767.0), -32768, 32767).astype(np.int16)
                vname = variant_sample_name(sample, i + 1, repr(key), taken)
                taken.add(vname.lower())
                renders[key] = vname
                stereo = FLAG_STEREO if y16.shape[1] == 2 else 0
                if a.format == 'adpcm':
                    data, chunk = ima_encode(y16)
                    out_samples.append((vname, rate, FLAG_ADPCM | FLAG_16BIT | stereo, chunk, data))
                else:
                    out_samples.append((vname, rate, FLAG_PCM | FLAG_16BIT | stereo, 0,
                                        y16.astype('<i2').tobytes()))
                variant_tokens[i].append(prefix + vname)
                if preview:
                    write_wav(os.path.join(preview, f'{sound}__{sample}__{i + 1}.wav'), y16, rate)
            report.append(f'  {sample}: {pcm.shape[0] / rate:.2f} s from {where}')

        for i, rep in enumerate(reps):
            r_air, r_geom = rep * cfg['cellmeters'], (ref + rep) * cfg['cellmeters']
            at = {fq: float(-20 * np.log10(max(1e-12, abs(response(np.array([fq]), r_air, r_geom, cfg,
                                                                  ground=False)[0]))))
                  for fq in (1000, 2000, 4000, 8000)}
            tau = ground_delay(r_geom, cfg['sourceheight'], cfg['listenerheight'], speed_of_sound(cfg['temperature']))
            notch = f'{1 / (2 * tau):.0f} Hz' if cfg['ground'] > 0 and tau > 0 else 'off'
            w = (i + 1) / len(bands)
            report.append(f'  {variants[i]}: beyond {bands[i]:g} cells, physics at {rep:g} cells (+{r_air:.0f} m of air, '
                          f'{r_geom:.0f} m away); '
                          f'air -{at[1000]:.1f}/-{at[2000]:.1f}/-{at[4000]:.1f}/-{at[8000]:.1f} dB at 1/2/4/8 kHz; '
                          f'ground notch {notch}; tail {cfg["tail"] * w:.2f} x T60 {cfg["tailtime"] * w:.2f} s')

        # Variant sections: the base's own keys, its Sounds= swapped for the
        # baked samples, Range widened, no Distance.* (they inherit the base's).
        base_items = [(k, v) for k, v, _ in ini.items(sound)
                      if k.lower() not in ('sounds', 'range') and not k.lower().startswith('distance.')]
        for i, vsec in enumerate(variants):
            body = [f'{MARK} generated by CosmeticExt tools/distance_bake.py from [{sound}] -- re-run the baker instead of editing',
                    'Sounds= ' + ' '.join(variant_tokens[i])]
            body += [f'{k}={v}' for k, v in base_items]
            body += [f'Range={var_range}']
            ini.append_section(vsec, body)
            soundlist_adds.append(vsec)

        adds = [f'Distance.Bands={",".join(variants)} {MARK}',
                f'Distance.Bands.From={",".join(f"{b:g}" for b in bands)} {MARK}']
        if ini.get(sound, 'Distance.Falloff') is None and global_falloff is None:
            adds.append(f'Distance.Falloff=inverse {MARK} bands need a Range-independent curve to join up')
            report.append('  Distance.Falloff=inverse added (no falloff set on the sound or in [CosmeticExt])')
        ini.insert(sound, adds)
        report.append('')

    if soundlist_adds:
        used = [int(k) for k, _, _ in ini.items('SoundList') if k.strip().isdigit()]
        nxt = (max(used) + 1) if used else 1
        ini.insert('SoundList', [f'{nxt + k}={n} {MARK}' for k, n in enumerate(soundlist_adds)])

    total = sum(len(s[4]) for s in out_samples)
    baked = len({v.rsplit('_D', 1)[0] for v in soundlist_adds})
    report.append(f'baked {baked} of {len(targets)} sound(s) -> {len(soundlist_adds)} variant(s), '
                  f'{len(out_samples)} sample(s), {total / 1048576:.1f} MiB in {bag_base}.bag ({a.format})')
    if problems:
        report += ['', 'problems:'] + [f'  {p}' for p in problems]

    if not a.dry_run:
        os.makedirs(a.out, exist_ok=True)
        if out_samples:
            write_bag(out_samples, os.path.join(a.out, bag_base + '.idx'), os.path.join(a.out, bag_base + '.bag'))
        with open(os.path.join(a.out, 'soundmd.ini'), 'wb') as f:
            f.write(ini.text().encode('latin1'))
        with open(os.path.join(a.out, 'bake-report.txt'), 'w') as f:
            f.write('\n'.join(report) + '\n')
    print('\n'.join(report))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
