# ztCamera against Samsung's camera app (Galaxy S9, One UI 2, G960FXXUHFVG4)

ROADMAP 8.78. What the stock app does, what ztOS can do on the same hardware without
Samsung's ISP, and in what order to do it. Written 2026-11-23.

**What ztOS has to work with:**
- `/dev/zrn-cam` gives RAW Bayer frames from CSIS, with no ISP.
  - Rear S5K2L3: preview 2016x1512 at 30 fps, full 4032x3024.
  - Front S5K3H1: preview 1632x1224, full 3264x2448.
- The GPU (Mali-G72 on Panfrost) develops every frame.
- The focus and aperture actuators answer. The rear flash LED is driven as a torch (`zrn-flash`, current codes 0-15).
- There is no OIS, no PDAF, and no sensor DRAM path.

The effort column uses S (hours), M (a day or two) and L (a week or more).

## Done in this pass (8.78)

| Feature | Stock | ztOS now | Notes |
| --- | --- | --- | --- |
| Front/rear switch | one tap | one tap, "switching…" while the sensor powers up | IR was a hidden third stop. The open now runs on a thread and retries once. |
| Auto white balance | ISP AWB with stock tuning | Grey-pixel, white-patch and grey-world estimates, smoothed, with a slight warm bias | Rear CCM is blended between stock's A and D65 forward matrices by the estimated light. |
| Auto exposure | multi-zone, face priority | Centre-weighted, or spot after a tap, with EV compensation ±3 | No faces. |
| Tone, colour | Samsung "rich" look | sRGB plus lifted shadows plus an S curve, saturation 1.18 (1.05 at high gain) | To tune against stock photos of the same scene. |
| Noise reduction, sharpening | ISP | GPU bilateral on luma and chroma, cored unsharp mask, both scaled by gain | Preview and stills alike. |
| Lens shading | per-module calibration (FROM) plus setfile tables | Radial gain per channel, red widest | Stock's tables are not decoded (see below). |
| Night / low light | "Bright Night" multi-frame | 4 full frames averaged on the GPU, with moving pixels left out, used automatically above ISO 200 | Not aligned, so hold still. |
| HDR (rich tone) | auto/on/off | auto/on/off: three exposures (0, -2, +1.5 EV) fused on the GPU | Auto triggers on a bright scene with >2 % clipped. |
| Pro mode | ISO, shutter, WB, focus, EV, aperture | The five dials drive the sensor: WB presets, focus (auto/macro/normal/infinity), ISO 100-3200, shutter 1/30-1/8000, EV; aperture auto, F1.5, F2.4 | Shutter longer than 1/30 needs a frame-length register write (0x0340), which is not approved. |
| Dual aperture | auto by light, manual in Pro | Auto by scene EV (F2.4 above 11, F1.5 below 9, at least 4 s apart) or manual | Stock's own writes (REGISTERS.md K). The driver's move limit is now a refilling bucket; it needs a reboot to load. |
| Tap to focus and expose | yes | yes: the focus window and spot metering move to the tap, for 8 s | Contrast AF only. |
| Flash | off/auto/on/torch | off/auto/on/torch | "Flash" is the torch at its top code (15) for about 1 s: dimmer than stock's real flash pulse, which would be a new S2MPB02 mode write (0x12 bits 7:6 = 10). |
| Timer | 2/5/10 s | 3/10 s | Already present. |
| Grid | 3x3 | 3x3 | Already present. |
| QR scanning | in the viewfinder | menu "QR scanner": libzbar (already on the phone) on the green channel, twice a second | Shows the text. Copy and open-link are not done yet. |
| Shooting info | Pro shows values | Status line: F-number, ISO, shutter, EV, fps | |

## Not done yet, by feasibility

| Feature | Stock | Feasible? | Effort | What it needs |
| --- | --- | --- | --- | --- |
| **Video recording** | UHD 60, FHD 60/30, HD | Yes | L | MFC v12 encoder: `media/` already brings up `s5p-mfc`, but only as out-of-tree modules for an older build. They need building for kernel #64 and loading, then a check that `/dev/video1` (s5p-mfc-enc) encodes H.264 on v12; only decode is proven. Then: developed RGBA, GPU RGB to NV12, a DMA-BUF into the MFC (no copy), and MP4 via the ffmpeg libs, with audio from ALSA (snd-zrn-abox). Fallback: x264 ultrafast at 720p30 on the big cores. The current raw-BGRA "video" records an empty frame for the real camera. |
| 60 fps video | FHD/UHD 60 | Maybe | M | A sensor mode at 60 fps: the 2L3's vendor tables have binned modes, but each is a new mode table to bring for approval. CSIS is fine. |
| Video stabilisation | VDIS (EIS) | Yes | M | Gyro from the sensor hub (`zrn-bbd`), GPU crop and warp per frame, about 10 % margin. |
| Burst | hold shutter, 10 fps | Yes | M | A full-size stream with GPU develop per frame. The PNG encode is the bottleneck: needs JPEG (libjpeg-turbo) on threads. |
| JPEG instead of PNG | JPEG plus EXIF | Yes | S | libjpeg-turbo plus EXIF (aperture, ISO, shutter, orientation). Saves 15-20 MB per photo. **Recommended next.** |
| Zoom | digital, to 8x (S9 has no telephoto) | Yes | S | Pinch to crop on the GPU. Above 2x, take the full-size mode's centre for the preview. |
| Live focus (bokeh) | portrait blur, single camera on S9 | Partial | L | No depth: either person segmentation (a small NN on CPU/GPU) or a PDAF depth map (PDAF VC is 4032x756 in the full mode, not captured yet). |
| Panorama | sweep | Yes | L | Gyro plus feature matching plus cylindrical warp; OpenCV is not on the phone. |
| Food | radial blur plus warm, saturated | Yes | S | A GPU shader preset over Photo. |
| Hyperlapse | timelapse with stabilisation | After video | M | Video plus frame dropping plus EIS. |
| Slow motion | 240 fps 720p | Unlikely | L | Needs a 240 fps sensor mode (exists in vendor tables) and CSIS at that rate. Each mode is a new register table to approve. |
| Super slow-mo | 960 fps, 0.2 s | No | - | Uses the 2L3's stacked DRAM and the vendor ISP's capture path. Not practical. |
| Scene optimiser | 20 scene classes | Partial | L | Needs a classifier model. A cheap version keys tone and saturation presets off simple stats (sky blue, green, warm). |
| Flaw detection | blink, blur, lens smudge | Partial | M | Blur is easy (the AF sharpness score). Blink needs faces. |
| Face detection / AF tracking | yes | Partial | L | Stock HFD model files are present (`HFD.model.new`) but run on the vendor DSP. A CPU detector (e.g. libfacedetection) at 320x240 would work. |
| AR Emoji, Bixby Vision, stickers | yes | No | - | Samsung services. |
| Beauty, filters (front) | yes | Yes | S-M | GPU shaders: skin smoothing is a bilateral on luma. |
| Ratios 3:4, 9:16, 1:1, full | yes | Yes | S | Crop at develop and in the viewfinder. |
| Long exposure (Pro, to 10 s) | yes | Needs approval | S | Frame length lines (0x0340) and coarse time above it: a new sensor register write. |
| Real flash pulse | yes | Needs approval | S | S2MPB02 0x12 flash mode and 0x14 flash current, and the timing. |
| RAW (DNG) save | Pro | Yes | S | Write the Bayer frame plus stock's forward matrices as a DNG. |
| Location tags | yes | Yes | S | The GNSS fix from the modem stack. |
| Voice/palm shutter | yes | Partial | M | Voice needs a local keyword model; palm needs a detector. |
| Iris camera | (Samsung Pass) | No | - | The secure world holds its pads (`camera/iris/README.md`). |

## Stock tuning data looked at (read-only, in the scratchpad)

- `libexynoscamera3.so`: sensor orientation, black and white level, max analog sensitivity, and the rear's forward matrices. All of these are already in use.
- `setfile_2l3.bin`, `setfile_3h1.bin`:
  - Both hold the same six 3x3 matrices over 256, at about 0x1818x-0x1884x: identity, three colour matrices, and a saturation matrix with diagonal 413 and off-diagonals -79.
  - The front's at 0x18557 is in use.
  - The tables round them (AWB locus, LSC grids) are in a layout not yet decoded. That is the main open item for colour accuracy.
- `fimc_is_rta_2l3_3h1.bin`: the 3A library. Auto aperture lives there (`CML_REAR::checkApertureChange`), with its thresholds in the setfile. The EV thresholds used here are therefore our own.

## Recommended order

1. JPEG plus EXIF (S): smaller, faster saves, and needed for burst.
2. Owner photo comparison: tune the WB presets, the tone curve and saturation against stock-phone photos of the same scenes (S per round).
3. Video through MFC (L): build and load s5p-mfc for #64, prove encode, then the NV12 path. Report first, as asked.
4. Zoom and ratios (S), Food and filters (S).
5. Video stabilisation and Hyperlapse (M) after video.
6. Face detection (L), then Live focus (L).
7. Approval-gated: long exposure (0x0340), real flash pulse (S2MPB02 0x12/0x14), 60 fps and slow-motion sensor modes.
