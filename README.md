# Lunar Desktop

Wireless PC streaming for Meta Quest, in the Lunar look. You can use your Windows desktop on a giant curved screen under a night sky, or play **PC VR games (SteamVR / OpenXR) like Gorilla Tag** wirelessly.

It has two parts:

| Part | Runs on | What it does |
|---|---|---|
| `LunarDesktop.exe` (+ SteamVR driver) | Your Windows PC | Captures the desktop or the SteamVR game, encodes it on the GPU (H.264/HEVC) and streams it over Wi-Fi. It also streams PC sound, receives the headset mic, and lets SteamVR see your Quest as a headset with Touch controllers. |
| `LunarDesktop-Quest.apk` | Quest 2 / 3 / 3S / Pro | Finds your PC, shows the stream with low-latency timewarp, and sends head and controller tracking back. |

---

## Install

### 1. Get the builds
Every push builds both parts on GitHub Actions. Open the repository's **Actions** tab, select the latest green **Build Lunar Desktop** run, and download:
- **LunarDesktop-Windows** (a folder with `LunarDesktop.exe`, `openvr_api.dll` and `driver\lunar\...`)
- **LunarDesktop-Quest** (`LunarDesktop-Quest.apk`)

If you push a tag such as `v1.0.0`, both files are also attached to a GitHub Release.

### 2. PC
1. Unzip **LunarDesktop-Windows** somewhere permanent, e.g. `C:\Lunar Desktop\`. Keep the `driver` folder next to the exe.
2. Install **SteamVR** from Steam (it's free) if you want to play VR games.
3. Run `LunarDesktop.exe`. A moon appears in the tray and the Lunar Desktop window opens.
   - On first run it registers its SteamVR driver automatically (SteamVR must be closed). You can also click **SteamVR → Register**.
   - If Windows Firewall asks, allow **Private networks**. If the headset can't find the PC later, click **Devices → Allow through firewall**.

### 3. Quest
1. Turn on **Developer Mode** for your headset in the Meta Horizon phone app.
2. Install the APK with [SideQuest](https://sidequestvr.com/) or `adb install -r LunarDesktop-Quest.apk`.
3. On the Quest, open **App Library → Unknown Sources → Lunar Desktop**.
4. Pick your PC from the list. The first time, click **Allow** in the Lunar Desktop window on the PC.

The headset reconnects automatically after that.

---

## Using it

**Desktop mode** (whenever no VR game is running)
- Point with either controller. **Trigger** = left click, **grip** = right click, **thumbstick** = scroll.
- **B / Y** opens the Lunar keyboard.
- The dock under the screen has buttons to switch monitors, open the keyboard, launch SteamVR, recenter, open settings and disconnect.
- **Short-press the left menu button** for settings. **Hold it** to recenter.

**VR mode** (switches on by itself as soon as SteamVR shows something)
- Everything goes to the game: head, both Touch controllers, all buttons, triggers, grips, sticks, touch sensors and haptics.
- **Hold the left menu button** to open the Lunar overlay. From there you can open the SteamVR menu, change bitrate or quality, exit SteamVR or disconnect. A short press still reaches the game as its menu button.
- When you close SteamVR, it goes back to the desktop.

---

## Playing Gorilla Tag (or any SteamVR game) wirelessly

1. Own **Gorilla Tag on Steam** (the PC version).
2. In Lunar Desktop on the PC, open the **SteamVR** tab and make sure every line has a green tick:
   - SteamVR installed
   - Lunar driver registered
   - **SteamVR is the OpenXR runtime.** Gorilla Tag uses OpenXR. If this line shows "!", open SteamVR → *Settings → OpenXR → Set SteamVR as OpenXR runtime*. If you also have the Meta Quest Link app, it may have taken over this setting.
3. Put on the headset and connect. Press the glowing **SteamVR** button on the dock (or *Launch SteamVR* on the PC).
4. Launch Gorilla Tag from Steam (on the PC or from the SteamVR menu).
5. **Voice chat:** install the free [VB-Audio Virtual Cable](https://vb-audio.com/Cable/). In Lunar Desktop → **Audio**, choose `CABLE Input`. Then set `CABLE Output` as your microphone, either in Gorilla Tag's settings or as the Windows default recording device. Your Quest's mic now reaches the game.

Tips for smooth play:
- Plug the **PC into the router with a cable**, and keep the Quest on **5 GHz or 6 GHz Wi-Fi** in the same room as the router.
- Start with **Quality: High, Bitrate: 100-150 Mbps, Codec: Auto, Refresh: 90**. If it stutters or smears, lower the bitrate first, then the quality.
- Lunar Desktop sets the SteamVR play area from your Quest boundary, so you won't get SteamVR's Room Setup. Your Quest guardian still protects you.
- If your hands feel slightly offset or tilted in a game, adjust **SteamVR → Controllers** in the PC window.

---

## Settings and themes

The PC window and the headset share settings. Change something in one and the other updates.

- **12 Lunar themes** (Lunar, Eclipse, Aurora, Blood Moon, Nebula, Frost, Harvest Moon, Sunset, Ocean, Forest, Sakura, Cyber) plus custom colours. The headset's sky, moon glow and glass follow the theme.
- **Glass look:** frost, tint and corners in the PC window, matching Lunar Music.
- **Clunk Mode:** the joke disguise skin (press **K** in the PC window). The headset turns plain and grey too.
- **Headset:** screen size, distance and curve; stars, big moon and the lunar ground.

PC settings are saved in `%AppData%\LunarDesktop\settings.json`. The log is `host.log` in the same folder (Lunar Desktop → About → Open log folder).

---

## How it works

```
Quest (OpenXR, native C++)                      PC (Windows, C++)
───────────────────────────                     ─────────────────────────────────────────
head + controller tracking ── UDP 47812 ──▶ LunarDesktop.exe ──shared memory──▶ driver_lunar.dll (in SteamVR)
                                                   ▲                                │ game renders
video (H.264/HEVC chunks)  ◀── UDP 47813 ──  MF hardware encoder ◀─ shared D3D11 ◀─┘ direct-mode compositor
PC sound (48 kHz PCM)      ◀── UDP 47813 ──  WASAPI loopback
microphone                 ── UDP 47812 ──▶  WASAPI playback → "CABLE Input"
control / pairing / input  ◀─ TCP 47811 ─▶   discovery on UDP 47810, settings UI on http://127.0.0.1:47800
```

- **Low latency:** the Quest sends its pose every frame. SteamVR renders with that pose, and the frame comes back tagged with it. The Quest submits it as a projection layer with that pose, so the headset's compositor timewarps it to your current head position. Turning your head stays smooth even with Wi-Fi delay.
- **Encoding** uses Windows Media Foundation hardware encoders, so it works on NVIDIA (NVENC), AMD (AMF/VCE) and Intel (Quick Sync) GPUs. RGB→NV12 conversion runs on the GPU. Big frames are paced so Wi-Fi doesn't drop bursts. A lost packet triggers an instant keyframe request.
- **Decoding** on the Quest uses MediaCodec in low-latency mode, straight into GPU textures through `AImageReader` and an `EGLImage`. No CPU copies.
- **Security:** a headset must be approved once on the PC before it can stream or control the mouse and keyboard. The settings page only answers on `127.0.0.1`.

### Repository layout

```
shared/          wire protocol + math shared by both sides
pc/host/         LunarDesktop.exe: networking, encoder, desktop capture, audio, input, tray, settings UI (ui/index.html)
pc/driver/       SteamVR driver: HMD + Touch controllers + direct-mode compositor
pc/common/       PC host <-> driver shared memory layout
quest/           Android/Quest app (Gradle + CMake, OpenXR, GLES 3)
assets/          Lunar logo, textures, icons, fonts and themes (from the Lunar Music kit)
```

### Building yourself
- **PC:** Visual Studio 2022 (or newer) with C++ and CMake. Run `cmake -S pc -B build -A x64`, then `cmake --build build --config Release`. The output is in `build/LunarDesktop`. A MinGW cross-compile from Linux also works.
- **Quest:** Android Studio, or JDK 17 plus the Android SDK. Run `cd quest && ./gradlew assembleRelease`. The APK is signed with the debug key so you can sideload it straight away.

---

## Troubleshooting

| Problem | Fix |
|---|---|
| Headset says "Searching…" forever | Make sure `LunarDesktop.exe` is running and both devices are on the same network. Allow it through the firewall (Devices tab), or use **Enter IP address** on the headset. The PC's IP is shown by `ipconfig`. |
| Headset stuck on "Allow this headset" | Click **Allow** in the Lunar Desktop window on the PC (the tray balloon opens it). |
| SteamVR says "Headset not detected" | Close SteamVR, click **Register** in the SteamVR tab, and start SteamVR again. Make sure the Quest is connected first. |
| A game opens on the monitor instead of in VR | That game uses OpenXR and another runtime is active. Set SteamVR as the OpenXR runtime (see above). |
| Picture stutters or smears | Lower the bitrate, use 5/6 GHz Wi-Fi, connect the PC by cable, or close other downloads. |
| Friends can't hear me | Install VB-Cable, choose `CABLE Input` in the Audio tab, and pick `CABLE Output` as the game's microphone. |
| No sound in the headset | Check **Audio → Play PC sound in the headset**. Lunar Desktop follows your Windows default output device. |

---

Uses the Lunar Music VR kit assets. Includes [Inter](https://rsms.me/inter/) (SIL OFL), [OpenVR](https://github.com/ValveSoftware/openvr) headers and `openvr_api.dll` (BSD-3), the [OpenXR loader](https://github.com/KhronosGroup/OpenXR-SDK) (Apache-2.0), [stb](https://github.com/nothings/stb) (public domain) and [nlohmann/json](https://github.com/nlohmann/json) (MIT).
