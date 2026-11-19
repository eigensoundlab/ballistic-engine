// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Daniel Sandner
using System;
using EigenSoundLab.Ballistic;

static class Program
{
    static int Main()
    {
        var s = Ballistic.Render("pistol", 3, "game", ("D", 0), ("env", "indoor"));
        s.Cut(0.5).SetPeak(0.5f);
        Console.WriteLine($"Ballistic Engine {Ballistic.Version}: pistol, {s.Seconds:0.000} s, {s.Events.Length} events, " +
                          $"first {s.Events[0]}, {Ballistic.Presets().Count} presets");
        if (s.Length != 24000 || s.Events[0].Kind != BallisticEventKind.Launch) return 1;
        var rail = Ballistic.Render("railgun", 2, null, ("D", 50));
        Console.WriteLine($"railgun: {rail.Seconds:0.000} s, {rail.Events.Length} events");
        try { Ballistic.Render("tesla"); return 1; }
        catch (BallisticException e) { Console.WriteLine("as it should: " + e.Message); }
        return 0;
    }
}
