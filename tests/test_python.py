# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Daniel Sandner
"""The Python binding (bindings/python), against the library in build/.  python -m unittest tests.test_python"""
import os
import sys
import tempfile
import unittest
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "bindings", "python"))
import ballistic_engine as be  # noqa: E402


class BindingTests(unittest.TestCase):
    def test_render_finish_cut_write(self):
        s = be.render("pistol", seed=3, D=0, env="indoor", action="game")
        self.assertEqual(s.rate, 48000)
        self.assertGreater(s.seconds, 0.3)
        self.assertEqual(s.events[0][0], "launch")
        self.assertLessEqual(max(map(abs, s.left)), 0.8901)
        s.cut(0.5).set_peak(0.5)
        self.assertEqual(s.n, 24000)
        self.assertAlmostEqual(max(max(map(abs, s.left)), max(map(abs, s.right))), 0.5, places=5)
        path = os.path.join(tempfile.mkdtemp(), "p.wav")
        s.write_wav(path)
        with wave.open(path) as w:
            self.assertEqual((w.getnchannels(), w.getframerate(), w.getsampwidth(), w.getnframes()), (2, 48000, 2, 24000))

    def test_same_seed_same_take(self):
        a, b, c = be.render("ricochet", seed=4), be.render("ricochet", seed=4), be.render("ricochet", seed=5)
        self.assertEqual(a.left, b.left)
        self.assertNotEqual(a.left, c.left)

    def test_threads_render_the_same_takes(self):
        from concurrent.futures import ThreadPoolExecutor
        jobs = [(p, seed) for p in ("pistol", "charge", "gravelblast") for seed in (1, 2, 3)]
        alone = [be.render(p, seed=seed, action="game").left for p, seed in jobs]
        with ThreadPoolExecutor(3) as pool:
            together = list(pool.map(lambda j: be.render(j[0], seed=j[1], action="game").left, jobs))
        self.assertEqual(alone, together)

    def test_errors_and_what_there_is(self):
        with self.assertRaisesRegex(be.BallisticError, "nope: no preset of that name"):
            be.render("nope")
        with self.assertRaisesRegex(be.BallisticError, "Dee: no parameter of that name"):
            be.render("pistol", Dee=1)
        with self.assertRaisesRegex(be.BallisticError, "cannot render that kind"):
            be.render("quad")
        with self.assertRaisesRegex(be.BallisticError, "oomph"):
            be.render("pistol", action="game", finish={"oomph": 1})
        listed = {p[0]: p for p in be.presets()}
        self.assertTrue(listed["pistol"][3])
        self.assertEqual(listed["charge"][2], "explosion")
        self.assertTrue(listed["tesla"][3])
        self.assertFalse(listed["quad"][3])
        self.assertTrue(listed["railgun"][3])
        self.assertFalse(listed["knockfire"][3])                 # needs a recording
        self.assertEqual(be.keys()["env"], "word")
        self.assertEqual(be.load().version, "0.3.0")

    def test_loops_stay_loops(self):
        s = be.render("shield", seed=2, action="game")
        self.assertTrue(s.loop)
        self.assertEqual(s.n, 4 * 48000)
        self.assertEqual(s.events, [])
        self.assertFalse(be.render("shielddown", seed=2).loop)


if __name__ == "__main__":
    unittest.main()
