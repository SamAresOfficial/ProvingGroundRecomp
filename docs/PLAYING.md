# Proving Ground Recompiled

A native PC version of **Tony Hawk's Proving Ground**, statically recompiled from the Xbox 360
release. It runs with the files of your own copy of the game; none are included.

## First start

1. Extract your Xbox 360 disc (an ISO dumped from your own copy) to a folder with any Xbox 360
   ISO extraction tool. The folder must contain `default.xex`.
2. Run `ProvingGround.exe` (Linux: `./ProvingGround`).
3. The launcher opens (in its own window on Windows). Under **Game folder**, click **Browse...** and choose
   that folder. "Game found and verified" means it is the version this build was made for.
4. Pick your options and press **Play** (or Enter). The launcher closes and the game starts.

If you put the extracted game in a folder named `game` next to the executable, the launcher
finds it by itself. To skip the launcher on later starts, turn off **Show this window at
startup**; start with `--launcher` to bring it back. Closing the launcher saves its settings.

### Linux and Steam Deck (work in progress)

The Linux build has not been tested on real hardware yet. On a Steam
Deck:

1. In desktop mode, extract the game into a folder named `game` next to `ProvingGround`, so the
   launcher finds it without the folder dialog. Browse... uses `kdialog` (included in the Deck's
   desktop mode) or `zenity`.
2. Start `./ProvingGround` once in desktop mode, check the options and press **Play**.
3. In Steam, **Add a Non-Steam Game** and pick `ProvingGround` to play it in game mode.

### Windows handhelds (ROG Xbox Ally and similar)

Not tested on a handheld yet. What to expect on a ROG Xbox Ally / Ally X:

- Put the extracted game in a folder named `game` next to `ProvingGround.exe`, so the launcher
  finds it without the folder dialog or typing a path.
- The launcher can be used with the built-in controller (D-pad or stick to move, A to change a
  setting, B to close a list, Start to play) or with the touch screen.
- In game, press **View + Menu** together (Back + Start on older controllers) to open the
  options menu. Move with the D-pad or left stick, change a setting with A, close a list with
  B, and press Menu (Start) or choose Resume to return to the game. While the menu is open the game doesn't see the controller.
- Suggested settings for the 1080p 120 Hz screen: Resolution 1920 x 1080 or 1280 x 720, Effects
  and HUD 1x, Sharpness about 50%, and a frame rate of 40 or 60 FPS (both divide 120 Hz evenly,
  so with Vsync on Automatic each frame is shown for exactly 3 or 2 refreshes, without tearing).
  Effects and HUD on Automatic picks 2x at 1080p, which renders them at 1440p and costs GPU time
  the small screen can't show.
- AMD FSR 3.1 (Anti-aliasing, Vulkan) renders below the screen's resolution and rebuilds the
  full image from several frames; it works on any GPU. See below.

## Options

| Option | |
| --- | --- |
| Language | The game's text and voices: English, Français, Deutsch, Italiano or Español. Automatic picks your system's language when the game has it, else English. |
| Display mode | Fullscreen or windowed. **F11** switches while playing. |
| Monitor | With more than one monitor: the one the game opens on. |
| Frame rate | 30 FPS keeps the original console pacing. 60 and above run the game faster than the console could; cutscenes still play at their original speed. |
| Vsync | Automatic (default): when the frame rate divides your screen's refresh rate evenly (30 or 60 FPS at 60 Hz; 30, 40, 60 or 120 at 120 Hz; 144 at 144 Hz), each frame waits for the screen, so there is no tearing and every frame is shown for the same time. Other rates and Unlimited are shown at once, for FreeSync / G-SYNC screens. On always waits for the screen; Off never does. Applies at once. |
| Renderer | Vulkan (main, fastest) or Direct3D 12 (Windows). Changing it in the Insert menu applies on the next start. |
| Resolution | What the 3D scene renders at: your screen's resolutions are listed, 1280 x 720 is the original. The game's image is 16:9 and fits inside the chosen size. With Vulkan it changes at once in the Insert menu; with Direct3D 12 it follows Effects and HUD. |
| Effects and HUD | What the effects (bloom, blur, the final image) and the HUD render at: 1x (720p), 2x (1440p), 3x (4K), or Automatic, the nearest one to the resolution. Lower is faster. Applies on the next start. |
| Texture filtering | Anisotropic filtering, Off to 16x (4x by default): keeps the ground and ramps sharp into the distance. |
| Anti-aliasing | Off, FXAA, or AMD FSR 3.1 (Vulkan), which renders at the Resolution and rebuilds the image at your screen's size from several frames. See below. |
| FSR quality | With AMD FSR: Native AA, Quality, Balanced, Performance or Ultra performance. It sets the Resolution for your screen. |
| Sharpness | AMD FSR 1 sharpening of the final image, Off by default. When the game's image is smaller than the window (Effects and HUD below your screen), FSR 1 also upscales it. Applies at once. |
| Button icons | Automatic, Xbox or PlayStation. See Controls. |
| Show FPS counter | A small counter in the top-right corner. |

Press **Insert** in game, or **View + Menu** (Back + Start) together on a controller, for the
options menu. Renderer, effects and HUD, anti-aliasing and button
icons apply on the next start; resolution, texture filtering and sharpness apply at once. Its
**Performance** tab lists the shortcuts the native renderer takes (all on by default); turning
one off shows whether it causes a glitch you see. They reset when the game closes.

About AMD FSR 3.1 (Anti-aliasing, with the Vulkan renderer): the game is rendered at the
Resolution, moved by a fraction of a pixel on every frame, and FSR combines the frames, with the
game's depth and camera motion, into an image at your screen's size. **FSR quality** picks the
resolution for you: Quality renders 1.5x fewer pixels in each direction, Balanced 1.7x,
Performance 2x, Ultra performance 3x, and Native AA only smooths edges at your screen's size.
With FSR on, the effects and the HUD render at the Resolution too (Effects and HUD is not used;
switching it takes a restart when the game was started at 2x or 3x), FXAA is off, and Sharpness
is FSR's own sharpening. With Direct3D 12 FSR is not offered: that renderer has no depth or
motion to give it, and the Sharpness slider (FSR 1) scales the image instead.

## Controls

Use an Xbox, PlayStation or other controller supported by SDL. It works as the Xbox 360
controller did; on a PlayStation controller, Cross is A, Circle is B, Square is X and Triangle
is Y.

**Button icons** (launcher and Insert menu) picks the icons the game shows: Automatic shows
PlayStation icons when a PlayStation controller is connected as the game starts.

**Keyboard**: on the launcher's **Controls** page, turn on **Play with the keyboard** to use the
keyboard as an Xbox 360 controller. Each controller button is listed with its keys; click one
and press the key you want (Escape cancels). Controller buttons themselves cannot be remapped.

## Files

| What | Where |
| --- | --- |
| Settings | `pg.toml` next to the executable |
| Saves and shader cache | `Documents\ProvingGroundRecomp` (Linux: `~/.local/share/ProvingGroundRecomp`) |
| Logs | `logs` next to the executable |

To keep everything next to the executable instead, create an empty `portable.txt` there.

## Troubleshooting

- **The game does not start**: update your GPU driver. The game renders with Vulkan, which
  GPUs from about 2016 onward support. The log in `logs` says what failed.
- **Stutter the first time you visit an area**: shaders are compiled and cached on first use.
  Later visits are smooth.
- **"Not the version this build was made for"**: the game may still work, but only the
  version listed in the project's README is tested.
