// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Daniel Sandner
// Ballistic Engine from C#: Unity, Godot (C#) or plain .NET, through P/Invoke.
//
//   var shot = Ballistic.Render("pistol", seed: 3, action: "game",
//                               ("D", 0), ("env", "indoor"));
//   shot.Cut(1.2);
//   AudioClip clip = shot.ToAudioClip("pistol-3");      // in Unity
//
// Rendering is thread-safe: render a pool of takes on worker threads (Task.Run) while a level loads, then make the
// AudioClips on the main thread. See bindings/csharp/README.md for where the native library goes per platform.
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

namespace EigenSoundLab.Ballistic
{
    public enum BallisticEventKind { Launch = 0, Crack = 1, Pass = 2, Impact = 3 }

    public struct BallisticEvent
    {
        public BallisticEventKind Kind;
        public double Time;
        public override string ToString() => $"{Kind} {Time:0.000} s";
    }

    public sealed class BallisticException : Exception
    {
        public readonly int Status;
        public BallisticException(int status, string what)
            : base((what != null ? what + ": " : "") + Native.Text(Native.be_status_text(status))) { Status = status; }
    }

    internal static class Native
    {
#if (UNITY_IOS || UNITY_WEBGL) && !UNITY_EDITOR
        const string Lib = "__Internal";       // compiled into the player from the C sources
#else
        const string Lib = "ballistic";
#endif

        [StructLayout(LayoutKind.Sequential)]
        internal struct Event { public int kind; public double t; }

        [StructLayout(LayoutKind.Sequential)]
        internal struct Sound
        {
            public IntPtr L, R;
            public int n, rate;
            public double peak_pa, spl;
            public IntPtr events;
            public int n_events;
        }

        [DllImport(Lib)] internal static extern IntPtr be_params_new();
        [DllImport(Lib)] internal static extern void be_params_free(IntPtr p);
        [DllImport(Lib)] internal static extern int be_params_preset(IntPtr p, byte[] id);
        [DllImport(Lib)] internal static extern int be_params_set(IntPtr p, byte[] key, double value);
        [DllImport(Lib)] internal static extern int be_params_set_text(IntPtr p, byte[] key, byte[] value);
        [DllImport(Lib)] internal static extern int be_render(IntPtr p, int seed, ref Sound s);
        [DllImport(Lib)] internal static extern int be_finish(ref Sound s, byte[] preset, IntPtr[] keys, double[] values, int n);
        [DllImport(Lib)] internal static extern void be_sound_free(ref Sound s);
        [DllImport(Lib)] internal static extern int be_kind_supported(byte[] kind);
        [DllImport(Lib)] internal static extern int be_preset_count();
        [DllImport(Lib)] internal static extern IntPtr be_preset_id(int i);
        [DllImport(Lib)] internal static extern IntPtr be_preset_name(int i);
        [DllImport(Lib)] internal static extern IntPtr be_preset_kind(int i);
        [DllImport(Lib)] internal static extern IntPtr be_status_text(int status);
        [DllImport(Lib)] internal static extern IntPtr be_version();

        internal static byte[] Z(string s) => s == null ? null : Encoding.UTF8.GetBytes(s + "\0");

        internal static string Text(IntPtr p)
        {
            if (p == IntPtr.Zero) return null;
            int n = 0;
            while (Marshal.ReadByte(p, n) != 0) n++;
            var b = new byte[n];
            Marshal.Copy(p, b, 0, n);
            return Encoding.UTF8.GetString(b);
        }

        internal static void Check(int status, string what)
        {
            if (status != 0) throw new BallisticException(status, what);
        }
    }

    /// <summary>A rendered sound: stereo, 48 kHz, float samples, and when its events happen.</summary>
    public sealed class BallisticSound
    {
        public const int Rate = 48000;
        public float[] Left, Right;
        public BallisticEvent[] Events;
        /// <summary>The loudest point at the listener before normalising, in dB SPL.</summary>
        public double Spl;

        public int Length => Left.Length;
        public double Seconds => (double)Left.Length / Rate;

        /// <summary>Cut to a length, fading the end (as the engine's be_cut).</summary>
        public BallisticSound Cut(double seconds, double fade = 0.04)
        {
            int n = Math.Max(1, (int)Math.Floor(seconds * Rate + 0.5));
            if (n >= Left.Length) return this;
            int f = Math.Min(n, (int)Math.Floor(fade * Rate + 0.5));
            Array.Resize(ref Left, n);
            Array.Resize(ref Right, n);
            for (int i = 0; i < f; i++)
            {
                float g = (float)((double)i / f);
                Left[n - 1 - i] *= g;
                Right[n - 1 - i] *= g;
            }
            return this;
        }

        /// <summary>Scale to a peak level (0..1).</summary>
        public BallisticSound SetPeak(float peak)
        {
            float pk = 0;
            for (int i = 0; i < Left.Length; i++) pk = Math.Max(pk, Math.Max(Math.Abs(Left[i]), Math.Abs(Right[i])));
            if (pk > 0)
                for (int i = 0; i < Left.Length; i++) { Left[i] *= peak / pk; Right[i] *= peak / pk; }
            return this;
        }

        /// <summary>The samples as L R L R..., as AudioClip.SetData wants them.</summary>
        public float[] Interleaved()
        {
            var x = new float[Left.Length * 2];
            for (int i = 0; i < Left.Length; i++) { x[2 * i] = Left[i]; x[2 * i + 1] = Right[i]; }
            return x;
        }

#if UNITY_5_3_OR_NEWER
        /// <summary>An AudioClip of the sound. Call on the main thread.</summary>
        public UnityEngine.AudioClip ToAudioClip(string name)
        {
            var clip = UnityEngine.AudioClip.Create(name, Left.Length, 2, Rate, false);
            clip.SetData(Interleaved(), 0);
            return clip;
        }
#endif
    }

    public static class Ballistic
    {
        public static string Version => Native.Text(Native.be_version());

        /// <summary>Every preset of Ballistic: id, name, kind, and whether this version of the engine renders it.</summary>
        public static List<(string id, string name, string kind, bool renders)> Presets()
        {
            var list = new List<(string, string, string, bool)>();
            for (int i = 0; i < Native.be_preset_count(); i++)
            {
                string kind = Native.Text(Native.be_preset_kind(i));
                list.Add((Native.Text(Native.be_preset_id(i)), Native.Text(Native.be_preset_name(i)), kind,
                          Native.be_kind_supported(Native.Z(kind)) != 0));
            }
            return list;
        }

        /// <summary>A take of a preset with some parameters changed - numbers, or words such as ("env", "indoor") -
        /// optionally with Ballistic's finishing for action ("game", "block", "slow", "far"). Thread-safe.</summary>
        public static BallisticSound Render(string preset, int seed = 1, string action = null,
                                            params (string key, object value)[] parameters)
        {
            IntPtr p = Native.be_params_new();
            if (p == IntPtr.Zero) throw new OutOfMemoryException();
            var s = new Native.Sound();
            try
            {
                if (preset != null) Native.Check(Native.be_params_preset(p, Native.Z(preset)), preset);
                foreach (var (key, value) in parameters)
                {
                    if (value is string text) Native.Check(Native.be_params_set_text(p, Native.Z(key), Native.Z(text)), key);
                    else if (value is bool flag) Native.Check(Native.be_params_set(p, Native.Z(key), flag ? 1 : 0), key);
                    else Native.Check(Native.be_params_set(p, Native.Z(key), Convert.ToDouble(value)), key);
                }
                Native.Check(Native.be_render(p, seed, ref s), preset ?? "render");
                if (action != null) Native.Check(Native.be_finish(ref s, Native.Z(action), null, null, 0), action);
                var sound = new BallisticSound { Left = new float[s.n], Right = new float[s.n], Spl = s.spl,
                                                 Events = new BallisticEvent[s.n_events] };
                Marshal.Copy(s.L, sound.Left, 0, s.n);
                Marshal.Copy(s.R, sound.Right, 0, s.n);
                int size = Marshal.SizeOf(typeof(Native.Event));
                for (int i = 0; i < s.n_events; i++)
                {
                    var e = (Native.Event)Marshal.PtrToStructure(new IntPtr(s.events.ToInt64() + i * size), typeof(Native.Event));
                    sound.Events[i] = new BallisticEvent { Kind = (BallisticEventKind)e.kind, Time = e.t };
                }
                return sound;
            }
            finally
            {
                Native.be_sound_free(ref s);
                Native.be_params_free(p);
            }
        }
    }
}
