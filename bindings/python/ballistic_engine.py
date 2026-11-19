# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Daniel Sandner
"""Ballistic Engine from Python, through ctypes: no compiler, no packages beyond the standard library.

    import ballistic_engine as be
    s = be.render("pistol", seed=3, D=0, env="indoor", action="game")
    s.cut(1.2)
    s.write_wav("pistol.wav")
    print(s.seconds, s.spl, s.events)       # [("launch", 0.03), ("pass", 0.07), ...]

The library is looked for in BALLISTIC_ENGINE_LIB, next to this file, in ../../build, then on the system path.
Python 3.8 or newer.
"""
from __future__ import annotations

import ctypes
import ctypes.util
import os
import sys
from array import array

__all__ = ["Engine", "Sound", "render", "presets", "keys", "load", "BallisticError"]

EVENTS = ("launch", "crack", "pass", "impact")


class BallisticError(Exception):
    """The engine said no: an unknown preset or parameter, a value it cannot take, a kind not ported yet."""

    def __init__(self, status, text, what=""):
        super().__init__("%s%s" % (what + ": " if what else "", text))
        self.status = status


class _Event(ctypes.Structure):
    _fields_ = [("kind", ctypes.c_int), ("t", ctypes.c_double)]


class _Sound(ctypes.Structure):
    _fields_ = [("L", ctypes.POINTER(ctypes.c_float)), ("R", ctypes.POINTER(ctypes.c_float)),
                ("n", ctypes.c_int), ("rate", ctypes.c_int), ("peak_pa", ctypes.c_double), ("spl", ctypes.c_double),
                ("events", ctypes.POINTER(_Event)), ("n_events", ctypes.c_int)]


def _names():
    if sys.platform == "win32":
        return ["ballistic.dll", "libballistic.dll"]
    if sys.platform == "darwin":
        return ["libballistic.dylib", "libballistic.0.dylib"]
    return ["libballistic.so", "libballistic.so.0"]


def _find():
    want = os.environ.get("BALLISTIC_ENGINE_LIB")
    if want:
        return want
    here = os.path.dirname(os.path.abspath(__file__))
    for base in (here, os.path.join(here, "..", "..", "build"), os.path.join(here, "..", "..", "build", "Release")):
        for name in _names():
            p = os.path.join(base, name)
            if os.path.isfile(p):
                return p
    return ctypes.util.find_library("ballistic")


class Engine:
    """The loaded library. ``load()`` gives a shared one."""

    def __init__(self, path=None):
        path = path or _find()
        if not path:
            raise OSError("the Ballistic Engine library was not found: build it (cmake -S . -B build && cmake --build "
                          "build), or set BALLISTIC_ENGINE_LIB to it")
        lib = ctypes.CDLL(path)
        P, S, C, D, I = ctypes.c_void_p, ctypes.POINTER(_Sound), ctypes.c_char_p, ctypes.c_double, ctypes.c_int
        sig = {
            "be_params_new": (P, []), "be_params_free": (None, [P]), "be_params_preset": (I, [P, C]),
            "be_params_set": (I, [P, C, D]), "be_params_set_text": (I, [P, C, C]),
            "be_params_get": (I, [P, C, ctypes.POINTER(D)]), "be_params_get_text": (C, [P, C]),
            "be_render": (I, [P, I, S]), "be_kind_supported": (I, [C]), "be_check": (I, [P]),
            "be_preset_supported": (I, [I]),
            "be_finish": (I, [S, C, ctypes.POINTER(C), ctypes.POINTER(D), I]),
            "be_cut": (None, [S, D, D]), "be_set_peak": (None, [S, D]), "be_write_wav": (I, [S, C, I]),
            "be_sound_free": (None, [S]), "be_preset_count": (I, []), "be_preset_id": (C, [I]),
            "be_preset_name": (C, [I]), "be_preset_kind": (C, [I]), "be_key_count": (I, []), "be_key_name": (C, [I]),
            "be_key_is_text": (I, [I]), "be_action_count": (I, []), "be_action_id": (C, [I]),
            "be_action_key_count": (I, []), "be_action_key_name": (C, [I]),
            "be_status_text": (C, [I]), "be_version": (C, []), "be_reference": (C, []),
        }
        for name, (res, args) in sig.items():
            fn = getattr(lib, name)
            fn.restype, fn.argtypes = res, args
        self.lib = lib
        self.path = path

    def check(self, status, what=""):
        if status:
            raise BallisticError(status, self.lib.be_status_text(status).decode(), what)

    @property
    def version(self):
        return self.lib.be_version().decode()

    @property
    def reference(self):
        return self.lib.be_reference().decode()

    def presets(self):
        """``[(id, name, kind, renders), ...]``: every preset of Ballistic, and whether this version renders it (a kind
        not ported yet, or a preset that needs a recording, does not)."""
        out = []
        for i in range(self.lib.be_preset_count()):
            out.append((self.lib.be_preset_id(i).decode(), self.lib.be_preset_name(i).decode(),
                        self.lib.be_preset_kind(i).decode(), self.lib.be_preset_supported(i) == 0))
        return out

    def actions(self):
        """``(presets, parameters)`` of Ballistic's finishing for action."""
        return ([self.lib.be_action_id(i).decode() for i in range(self.lib.be_action_count())],
                [self.lib.be_action_key_name(i).decode() for i in range(self.lib.be_action_key_count())])

    def keys(self):
        """``{name: "number" or "word"}``: the parameters."""
        return {self.lib.be_key_name(i).decode(): "word" if self.lib.be_key_is_text(i) else "number"
                for i in range(self.lib.be_key_count())}

    def render(self, preset=None, seed=1, action=None, finish=None, **params):
        """A take of a preset with some parameters changed, optionally finished (``action="game"``, and
        ``finish={"punch": 0.8}`` over it)."""
        p = self.lib.be_params_new()
        if not p:
            raise MemoryError()
        try:
            if preset:
                self.check(self.lib.be_params_preset(p, preset.encode()), preset)
            for key, value in params.items():
                if isinstance(value, str):
                    self.check(self.lib.be_params_set_text(p, key.encode(), value.encode()), key)
                else:
                    self.check(self.lib.be_params_set(p, key.encode(), float(value)), key)
            raw = _Sound()
            self.check(self.lib.be_render(p, int(seed), ctypes.byref(raw)), preset or "render")
        finally:
            self.lib.be_params_free(p)
        sound = Sound(self, raw)
        if action or finish:
            sound.finish(action, **(finish or {}))
        return sound


class Sound:
    """A rendered sound: stereo, 48 kHz, float. ``left`` and ``right`` copy the samples out as ``array('f')``."""

    def __init__(self, engine, raw):
        self._e = engine
        self._raw = raw

    def __del__(self):
        try:
            self._e.lib.be_sound_free(ctypes.byref(self._raw))
        except Exception:
            pass

    @property
    def n(self):
        return self._raw.n

    @property
    def rate(self):
        return self._raw.rate

    @property
    def seconds(self):
        return self._raw.n / float(self._raw.rate)

    @property
    def spl(self):
        """The loudest point at the listener before it was normalised, in dB SPL."""
        return self._raw.spl

    @property
    def events(self):
        return [(EVENTS[e.kind] if 0 <= e.kind < len(EVENTS) else str(e.kind), e.t)
                for e in (self._raw.events[i] for i in range(self._raw.n_events))]

    @property
    def left(self):
        return array("f", ctypes.cast(self._raw.L, ctypes.POINTER(ctypes.c_float * self.n)).contents)

    @property
    def right(self):
        return array("f", ctypes.cast(self._raw.R, ctypes.POINTER(ctypes.c_float * self.n)).contents)

    def finish(self, action="game", **params):
        known = self._e.actions()[1]
        for k in params:
            if k not in known:
                raise BallisticError(1, "no action parameter of that name (there are: %s)" % ", ".join(known), k)
        keys = (ctypes.c_char_p * max(1, len(params)))(*[k.encode() for k in params])
        values = (ctypes.c_double * max(1, len(params)))(*[float(v) for v in params.values()])
        self._e.check(self._e.lib.be_finish(ctypes.byref(self._raw), action.encode() if action else None,
                                            keys, values, len(params)), action or "finish")
        return self

    def cut(self, seconds, fade=0.04):
        self._e.lib.be_cut(ctypes.byref(self._raw), float(seconds), float(fade))
        return self

    def set_peak(self, peak):
        self._e.lib.be_set_peak(ctypes.byref(self._raw), float(peak))
        return self

    def write_wav(self, path, bits=16):
        self._e.check(self._e.lib.be_write_wav(ctypes.byref(self._raw), os.fsencode(path), int(bits)), str(path))
        return path


_shared = None


def load(path=None):
    global _shared
    if path:
        return Engine(path)
    if _shared is None:
        _shared = Engine()
    return _shared


def render(preset=None, seed=1, action=None, finish=None, **params):
    return load().render(preset, seed, action, finish, **params)


def presets():
    return load().presets()


def keys():
    return load().keys()
