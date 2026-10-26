#!/usr/bin/env python3
"""Tests for tools/distance_bake.py. No game files needed: every fixture is
synthesised here.   python3 -m unittest tests/test_distance_bake.py"""

import contextlib
import io
import math
import os
import struct
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
import distance_bake as db  # noqa: E402

FS = 22050


def chirp(seconds=1.0, amp=0.5, seed=1):
    t = np.arange(int(FS * seconds)) / FS
    rng = np.random.default_rng(seed)
    return amp * np.sin(2 * np.pi * (150 + 2000 * t) * t) + 0.15 * rng.standard_normal(len(t)) * np.exp(-3 * t)


def snr_db(ref, got):
    ref = ref.astype(float)
    err = ref - got.astype(float)
    return 10 * math.log10(np.mean(ref ** 2) / max(1e-12, np.mean(err ** 2)))


def read(path):
    with open(path, 'rb') as f:
        return f.read()


def to16(x):
    return np.clip(np.round(x * 32767), -32768, 32767).astype(np.int16)


def fir_gain_db(h, fs, f):
    """Magnitude of a FIR at frequency f, dB."""
    n = np.arange(len(h))
    return 20 * math.log10(abs(np.sum(h * np.exp(-2j * np.pi * f * n / fs))))


class Physics(unittest.TestCase):
    # ISO 9613-2:1996 Table 2 (computed from ISO 9613-1), dB/km at the exact
    # octave midband frequencies 63..8000 Hz.
    TABLE = {
        (10, 70): [0.1, 0.4, 1.0, 1.9, 3.7, 9.7, 32.8, 117],
        (15, 20): [0.3, 0.6, 1.2, 2.7, 8.2, 28.2, 88.8, 202],
        (15, 50): [0.1, 0.5, 1.2, 2.2, 4.2, 10.8, 36.2, 129],
        (15, 80): [0.1, 0.3, 1.1, 2.4, 4.1, 8.3, 23.7, 82.8],
        (20, 70): [0.1, 0.3, 1.1, 2.8, 5.0, 9.0, 22.9, 76.6],
        (30, 70): [0.1, 0.3, 1.0, 3.1, 7.4, 12.7, 23.1, 59.3],
    }

    def test_iso9613_table(self):
        mids = [1000 * 10 ** (3 * k / 10) for k in range(-4, 4)]
        for (t, rh), row in self.TABLE.items():
            for f, ref in zip(mids, row):
                got = float(db.air_absorption(f, t, rh)) * 1000
                # the table prints one decimal (three significant figures)
                self.assertLessEqual(abs(got - ref), 0.05 + 0.005 * ref, f'{t} C {rh} % {f:.0f} Hz')

    def test_ground_delay_geometry(self):
        c = db.speed_of_sound(20)
        self.assertAlmostEqual(c, 343.2, delta=0.2)
        # far field: path difference ~ 2 hs hr / r
        tau = db.ground_delay(1000, 2, 30, c)
        self.assertAlmostEqual(tau * c, 2 * 2 * 30 / 1000, delta=1e-3)

    def test_comb_notch_and_energy(self):
        cfg = dict(db.BAKE_DEFAULTS, air=0.0, ground=0.5)
        tau = db.ground_delay(400, cfg['sourceheight'], cfg['listenerheight'], db.speed_of_sound(20))
        notch = 1 / (2 * tau)
        f = np.linspace(1, 20 * notch, 200001)
        H = np.abs(db.response(f, 0.0, 400, cfg))
        self.assertAlmostEqual(float(f[np.argmin(H[f < 1.5 * notch])]), notch, delta=notch * 0.01)
        self.assertAlmostEqual(float(H.min()), 0.5 / math.sqrt(1.25), delta=1e-3)
        self.assertAlmostEqual(float(np.mean(H ** 2)), 1.0, delta=0.01)  # energy-normalised

    def test_fir_matches_response(self):
        cfg = dict(db.BAKE_DEFAULTS)
        for r_air in (100, 600, 1500):
            h, delay = db.design_fir(FS, r_air, r_air + 200, cfg, ground=False)
            self.assertEqual(delay, len(h) // 2)
            for f in (500, 1000, 2000, 4000, 8000):
                want = 20 * math.log10(abs(db.response(np.array([f]), r_air, 0, cfg, ground=False)[0]))
                if want > -45:  # Blackman sidelobes floor the stopband near -58 dB
                    self.assertAlmostEqual(fir_gain_db(h, FS, f), want, delta=0.5, msg=f'{r_air} m {f} Hz')

    def test_no_air_at_zero_extra_path(self):
        cfg = dict(db.BAKE_DEFAULTS, ground=0.0)
        H = db.response(np.array([1000.0, 8000.0]), 0.0, 500, cfg)
        np.testing.assert_allclose(np.abs(H), 1.0)

    def test_variant_is_time_aligned(self):
        x = np.zeros((4000, 1))
        x[1000, 0] = 0.9
        for ground in (0.0, 0.5):
            cfg = dict(db.BAKE_DEFAULTS, ground=ground, tail=0.0)
            y, gain = db.bake_signal(x, FS, 600, 800, 1, 3, cfg, 'imp')
            self.assertEqual(int(np.argmax(np.abs(y[:, 0]))), 1000, f'ground {ground}')
            self.assertEqual(gain, 1.0)

    def test_far_band_removes_top_octave(self):
        x = chirp(1.0)[:, None]
        cfg = dict(db.BAKE_DEFAULTS, ground=0.0, tail=0.0)
        y, _ = db.bake_signal(x, FS, 1250, 1450, 2, 3, cfg, 'c')

        def band(z, lo, hi):
            Z = np.abs(np.fft.rfft(z[:len(x), 0] * np.hanning(len(x)))) ** 2
            f = np.fft.rfftfreq(len(x), 1 / FS)
            return 10 * math.log10(Z[(f >= lo) & (f < hi)].sum())
        self.assertLess(band(y, 6400, 11000) - band(x, 6400, 11000), -40)

    def test_tones_lose_what_iso9613_predicts(self):
        cfg = dict(db.BAKE_DEFAULTS, ground=0.0, tail=0.0)
        t = np.arange(FS) / FS
        for f in (250, 1000, 2000, 4000):
            x = (0.5 * np.sin(2 * np.pi * f * t))[:, None]
            y, _ = db.bake_signal(x, FS, 1250, 1450, 2, 3, cfg, 'tone')
            mid = slice(FS // 4, 3 * FS // 4)  # steady state, away from the FIR's edges
            got = 20 * math.log10(np.std(y[mid, 0]) / np.std(x[mid, 0]))
            want = -float(db.air_absorption(f)) * 1250
            self.assertAlmostEqual(got, want, delta=0.2, msg=f'{f} Hz')

    def test_tail_extends_and_is_deterministic(self):
        x = chirp(0.5)[:, None]
        cfg = dict(db.BAKE_DEFAULTS)
        a, _ = db.bake_signal(x, FS, 1250, 1450, 2, 3, cfg, 'seed')
        b, _ = db.bake_signal(x, FS, 1250, 1450, 2, 3, cfg, 'seed')
        c, _ = db.bake_signal(x, FS, 1250, 1450, 2, 3, cfg, 'other')
        np.testing.assert_array_equal(a, b)
        self.assertFalse(np.array_equal(a, c))
        self.assertGreater(len(a), len(x) + FS // 2)    # T60 1.4 s tail, trimmed at -60 dB
        self.assertEqual(float(a[-1, 0]), 0.0)          # faded, no click

    def test_clip_guard(self):
        x = np.full((2000, 1), 0.999)
        x[::2] = -0.999
        cfg = dict(db.BAKE_DEFAULTS, air=0.0, tail=0.0, ground=0.9)
        y, gain = db.bake_signal(x, FS, 0, 100, 0, 3, cfg, 'clip')
        self.assertLessEqual(float(np.max(np.abs(y))), 0.98 + 1e-9)
        self.assertLess(gain, 1.0)


class Codec(unittest.TestCase):
    def test_ima_mono_roundtrip(self):
        pcm = to16(chirp(2.0))[:, None]
        data, chunk = db.ima_encode(pcm)
        self.assertEqual(chunk, 512)
        self.assertEqual(len(data) % chunk, 0)
        dec = db.ima_decode(data, chunk, 1)
        self.assertGreaterEqual(len(dec), len(pcm))
        self.assertGreater(snr_db(pcm[:, 0], dec[:len(pcm), 0]), 15)

    def test_ima_stereo_roundtrip_keeps_channels(self):
        left = to16(chirp(1.0, seed=2))
        right = to16(0.4 * np.sin(2 * np.pi * 330 * np.arange(FS) / FS))
        pcm = np.stack([left, right], axis=1)
        data, chunk = db.ima_encode(pcm)
        self.assertEqual(chunk, 1024)
        dec = db.ima_decode(data, chunk, 2)[:FS]
        self.assertGreater(snr_db(left, dec[:, 0]), 15)
        self.assertGreater(snr_db(right, dec[:, 1]), 15)

    def test_decode_pcm_layouts(self):
        s16 = np.array([[0, 1], [-2, 32767], [-32768, 5]], dtype=np.int16)
        got = db.decode_entry(s16.astype('<i2').tobytes(), db.FLAG_PCM | db.FLAG_16BIT | db.FLAG_STEREO, 0)
        np.testing.assert_array_equal(got, s16)
        got8 = db.decode_entry(bytes([128, 255, 0]), db.FLAG_PCM, 0)
        np.testing.assert_array_equal(got8[:, 0], [0, 127 << 8, -128 << 8])

    def test_idx_roundtrip_sorted(self):
        with tempfile.TemporaryDirectory() as d:
            samples = [('Zed', FS, 6, 0, b'\1\0' * 3), ('alpha', FS, 12, 512, b'\0' * 512),
                       ('Beta', 44100, 7, 0, b'\2\0' * 4)]
            db.write_bag(samples, os.path.join(d, 'a.idx'), os.path.join(d, 'a.bag'))
            entries = db.read_idx(read(os.path.join(d, 'a.idx')))
            bag = read(os.path.join(d, 'a.bag'))
            self.assertEqual([e[0] for e in entries], ['alpha', 'Beta', 'Zed'])
            self.assertEqual(entries[1][3:], (44100, 7, 0))
            self.assertEqual(bag[entries[2][1]:entries[2][1] + entries[2][2]], b'\1\0' * 3)
            self.assertEqual(sum(e[2] for e in entries), len(bag))
            with self.assertRaises(ValueError):
                db.write_bag([('sixteen_chars_xx', FS, 6, 0, b'')], os.path.join(d, 'b.idx'),
                             os.path.join(d, 'b.bag'))

    def test_wav_roundtrip(self):
        with tempfile.TemporaryDirectory() as d:
            pcm = np.stack([to16(chirp(0.2)), to16(-chirp(0.2))], axis=1)
            p = os.path.join(d, 'x.wav')
            db.write_wav(p, pcm, FS)
            got, rate = db.read_wav(read(p))
            self.assertEqual(rate, FS)
            np.testing.assert_array_equal(got, pcm)


class Names(unittest.TestCase):
    def test_variant_names(self):
        taken = set()
        self.assertEqual(db.variant_sample_name('gexp14a', 1, 'k', taken), 'gexp14a_d1')
        long_name = db.variant_sample_name('fifteen_chars_x', 2, 'k', taken)
        self.assertLessEqual(len(long_name), db.NAME_MAX)
        self.assertRegex(long_name, r'^ce[0-9a-f]{9}_2$')
        taken.add('gexp14a_d1')
        other = db.variant_sample_name('gexp14a', 1, 'k2', taken)
        self.assertNotEqual(other.lower(), 'gexp14a_d1')
        taken.add(other.lower())
        third = db.variant_sample_name('gexp14a', 1, 'k2', taken)
        self.assertNotIn(third.lower(), taken)  # never collides, even for the same key
        self.assertLessEqual(len(db.variant_sample_name('x' * 15, db.MAX_BANDS, 'k', set())), db.NAME_MAX)

    def test_written_by_us(self):
        def idx(names):
            out = struct.pack('<4sII', b'GABA', 2, len(names))
            for n in names:
                out += struct.pack('<16siiIII', n.encode(), 0, 0, FS, 6, 0)
            return out
        self.assertTrue(db.written_by_us(idx(['gexp14a_d1', 'ce0123456ab_3'])))
        self.assertFalse(db.written_by_us(idx(['gexp14a_d1', 'myboom'])))
        self.assertFalse(db.written_by_us(b'nope'))


SOUNDMD = '''; test soundmd\r
[Defaults]\r
Range=10\r
Volume=80\r
\r
[SoundList]\r
1=Boom\r
2=Quiet\r
3=Wide\r
4=Broken\r
\r
[CosmeticExt]\r
Distance.Bake.Tail=0.2\r
\r
[Boom]\r
Sounds= $boom1 boom1 ; listed twice on purpose\r
Distance.Bake=yes\r
Control= random\r
Volume=70\r
\r
[Quiet]\r
Sounds= boom1\r
\r
[Wide]\r
Sounds= boom2 fifteen_chars_x\r
Range=20\r
Distance.Bake=yes\r
Distance.Bake.Bands=2,10\r
Distance.Falloff=linear\r
\r
[Broken]\r
Sounds= missing1\r
Distance.Bake=yes\r
\r
'''


class EndToEnd(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = self.tmp.name
        self.audio = os.path.join(self.d, 'audio')
        os.makedirs(self.audio)
        mono = to16(chirp(0.6, seed=3))[:, None]
        stereo = np.stack([to16(chirp(0.4, seed=4)), to16(chirp(0.4, seed=5))], axis=1)
        st_data, st_chunk = db.ima_encode(stereo)
        db.write_bag([('boom1', FS, db.FLAG_PCM | db.FLAG_16BIT, 0, mono.astype('<i2').tobytes()),
                      ('boom2', FS, db.FLAG_ADPCM | db.FLAG_16BIT | db.FLAG_STEREO, st_chunk, st_data)],
                     os.path.join(self.audio, 'audio.idx'), os.path.join(self.audio, 'audio.bag'))
        db.write_wav(os.path.join(self.audio, 'fifteen_chars_x.wav'), to16(chirp(0.3, seed=6))[:, None], FS)
        self.ini = os.path.join(self.d, 'soundmd.ini')
        with open(self.ini, 'wb') as f:
            f.write(SOUNDMD.encode('latin1'))

    def tearDown(self):
        self.tmp.cleanup()

    def bake(self, soundmd, out, *extra):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
            rc = db.main(['--soundmd', soundmd, '--search', self.audio, '--out', out] + list(extra))
        return rc, buf.getvalue()

    def test_bake(self):
        out = os.path.join(self.d, 'out')
        rc, log = self.bake(self.ini, out)
        self.assertEqual(rc, 1, log)  # [Broken] is a reported problem ...
        self.assertIn('missing1 not found', log)
        ini = db.Ini(read(os.path.join(out, 'soundmd.ini')).decode('latin1'))

        # ... but everything else was baked
        self.assertEqual(ini.get('Boom', 'Distance.Bands'), 'Boom_D1,Boom_D2,Boom_D3')
        self.assertEqual(ini.get('Boom', 'Distance.Bands.From'), '4,14,32')
        self.assertEqual(ini.get('Boom', 'Distance.Falloff'), 'inverse')
        self.assertEqual(ini.get('Boom_D1', 'Sounds'), '$boom1_d1 boom1_d1')  # one render, both tokens
        self.assertEqual(ini.get('Boom_D2', 'Volume'), '70')
        self.assertEqual(ini.get('Boom_D2', 'Control'), 'random')
        self.assertEqual(ini.get('Boom_D3', 'Range'), '74')
        self.assertIsNone(ini.get('Boom_D1', 'Distance.Bake'))
        self.assertEqual(ini.get('Wide', 'Distance.Bands.From'), '2,10')
        self.assertEqual(ini.get('Wide', 'Distance.Falloff'), 'linear')  # an author's choice is kept
        self.assertEqual(ini.get('Wide_D2', 'Range'), '30')
        self.assertFalse(ini.has('Broken_D1'))
        self.assertIsNone(ini.get('Quiet', 'Distance.Bands'))
        wide1 = ini.get('Wide_D1', 'Sounds').split()
        self.assertEqual(wide1[0], 'boom2_d1')
        self.assertRegex(wide1[1], r'^ce[0-9a-f]{9}_1$')
        self.assertEqual([v for _, v, _ in ini.items('SoundList')],
                         ['Boom', 'Quiet', 'Wide', 'Broken', 'Boom_D1', 'Boom_D2', 'Boom_D3', 'Wide_D1', 'Wide_D2'])
        self.assertEqual([k for k, _, _ in ini.items('SoundList')][4:], ['5', '6', '7', '8', '9'])

        # nothing of the input was lost or reordered
        src = SOUNDMD.replace('\r', '').split('\n')
        got = read(os.path.join(out, 'soundmd.ini')).decode('latin1').split('\r\n')
        it = iter(got)
        for line in [l for l in src if l.strip()]:
            self.assertTrue(any(g == line for g in it), f'lost or moved: {line!r}')
        self.assertTrue(got[-2].startswith('; EOF guard'))

        # the bag decodes, holds exactly the referenced samples, and is PCM16
        entries = db.read_idx(read(os.path.join(out, 'audio97.idx')))
        names = {e[0] for e in entries}
        referenced = {t.lstrip('$#') for s in ('Boom_D1', 'Boom_D2', 'Boom_D3', 'Wide_D1', 'Wide_D2')
                      for t in ini.get(s, 'Sounds').split()}
        self.assertEqual(names, referenced)
        bag = read(os.path.join(out, 'audio97.bag'))
        for name, off, size, rate, flags, chunk in entries:
            self.assertEqual(rate, FS)
            self.assertTrue(flags & db.FLAG_PCM and flags & db.FLAG_16BIT)
            pcm = db.decode_entry(bag[off:off + size], flags, chunk)
            self.assertEqual(pcm.shape[1], 2 if name.startswith('boom2') else 1)
            self.assertGreater(int(np.max(np.abs(pcm))), 1000)

        # re-running on its own output changes nothing, even with the old bag
        # deployed where the samples are searched
        out2 = os.path.join(self.d, 'out2')
        os.replace(os.path.join(out, 'audio97.idx'), os.path.join(self.audio, 'audio97.idx'))
        os.replace(os.path.join(out, 'audio97.bag'), os.path.join(self.audio, 'audio97.bag'))
        rc2, log2 = self.bake(os.path.join(out, 'soundmd.ini'), out2)
        self.assertEqual(rc2, 1, log2)
        self.assertIn('stripped', log2)
        self.assertEqual(read(os.path.join(out, 'soundmd.ini')),
                         read(os.path.join(out2, 'soundmd.ini')))
        self.assertEqual(read(os.path.join(self.audio, 'audio97.bag')),
                         read(os.path.join(out2, 'audio97.bag')))

    def test_adpcm_format(self):
        out = os.path.join(self.d, 'out')
        self.bake(self.ini, out, '--format', 'adpcm', '--only', 'Boom')
        entries = db.read_idx(read(os.path.join(out, 'audio97.idx')))
        self.assertEqual(sorted(e[0] for e in entries), ['boom1_d1', 'boom1_d2', 'boom1_d3'])
        self.assertTrue(all(e[4] == db.FLAG_ADPCM | db.FLAG_16BIT and e[5] == 512 for e in entries))

    def test_refuses_foreign_bag(self):
        db.write_bag([('someonesboom', FS, 6, 0, b'\0\0')], os.path.join(self.audio, 'audio97.idx'),
                     os.path.join(self.audio, 'audio97.bag'))
        out = os.path.join(self.d, 'out')
        rc, log = self.bake(self.ini, out)
        self.assertEqual(rc, 2)
        self.assertIn('not written by this tool', log)
        self.assertFalse(os.path.exists(out))
        rc, _ = self.bake(self.ini, out, '--bag', '96')
        self.assertEqual(rc, 1)  # only [Broken]
        self.assertTrue(os.path.exists(os.path.join(out, 'audio96.bag')))

    def test_dry_run_writes_nothing(self):
        out = os.path.join(self.d, 'out')
        rc, log = self.bake(self.ini, out, '--dry-run')
        self.assertIn('baked 2 of 3 sound(s) -> 5 variant(s)', log)
        self.assertFalse(os.path.exists(out))


if __name__ == '__main__':
    unittest.main()
