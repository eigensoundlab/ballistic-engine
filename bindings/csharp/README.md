# Ballistic Engine in Unity and .NET

`BallisticEngine.cs` is the whole binding: copy it into the project (in Unity, anywhere under `Assets/`) and add the
native library for each platform the game ships on.

Each release has `ballistic-engine-VERSION-unity.zip`: unpack it into `Assets/` and the binding and the native
libraries for Windows (x64, ARM64), Linux, macOS (universal) and Android (arm64-v8a, armeabi-v7a, x86_64) are in
place, with the C sources for iOS and WebGL. Check each plugin's platform in the Inspector once (Unity guesses from
the folder; `x86_64` holds both the Windows `.dll` and the Linux `.so`). To build them yourself:

| Platform | Native library | Where in Unity |
| --- | --- | --- |
| Windows | `ballistic.dll` (`cmake --build build --config Release`) | `Assets/Plugins/x86_64/` |
| macOS | `libballistic.dylib` (rename to `ballistic.bundle`, or keep `.dylib`) | `Assets/Plugins/macOS/` |
| Linux | `libballistic.so` | `Assets/Plugins/x86_64/` |
| Android | `libballistic.so` per ABI, built with the NDK's CMake toolchain | `Assets/Plugins/Android/libs/arm64-v8a/` ... |
| iOS, WebGL | the C sources: `include/ballistic.h`, `src/*.c`, `src/*.h` (Unity compiles them into the player) | `Assets/Plugins/iOS/`, `Assets/Plugins/WebGL/` |

On iOS and WebGL the binding calls `__Internal` (set by `UNITY_IOS` / `UNITY_WEBGL`). Where Unity compiles the C
sources, keep `-ffp-contract=off` if the platform lets you set it; without it a take may differ from Ballistic's by
rounding (inaudibly), but stays the same take on every run.

```csharp
using System.Threading.Tasks;
using EigenSoundLab.Ballistic;
using UnityEngine;

public class Pistol : MonoBehaviour
{
    public AudioSource source;
    AudioClip[] takes;
    int next;

    async void Start()
    {
        // render six takes off the main thread, then make the clips on it
        var sounds = await Task.Run(() =>
        {
            var list = new BallisticSound[6];
            for (int i = 0; i < 6; i++)
                list[i] = Ballistic.Render("pistol", seed: i + 1, action: "game",
                                           ("D", 0), ("target", "none"), ("env", "indoor")).Cut(1.2);
            return list;
        });
        takes = new AudioClip[sounds.Length];
        for (int i = 0; i < sounds.Length; i++) takes[i] = sounds[i].ToAudioClip("pistol-" + i);
    }

    public void Fire()
    {
        if (takes == null) return;
        source.PlayOneShot(takes[next]);
        next = (next + 1) % takes.Length;
    }
}
```

Each render returns its events (`Launch`, `Crack`, `Pass`, `Impact` and when they happen), for lining up a muzzle
flash or a hit decal with the sound.

Outside Unity the same file works in any .NET 6+ program (`tests/csharp` is one, run in CI); the library must be on
the loader's path (`LD_LIBRARY_PATH`, `DYLD_LIBRARY_PATH`, or next to the program).
