# ztmirror

The device's screen and fingers over one TCP socket, like scrcpy. The same
program serves the phone (USB-NCM) and QEMU (a `hostfwd` port).

    installer/ztos mirror --target phone     # or qemu: a window, the mouse as a finger
    installer/ztos touch tap 180 400         # scripted, in points
    installer/ztos mirror --snapshot out.png
    installer/ztos mirror --stats 5

`ztos mirror` starts `ztmirror.service` over SSH, reads its token, and stops it
at the end. SSH is used for nothing else. Video and input go directly over TCP.

To run it without SSH, start `ztmirror` on the device and pass `--token`, whose
value is in `$XDG_RUNTIME_DIR/ztmirror.token`.

## On this computer

The window needs PySide6 (`python3-pyside6` on Fedora/EL, `pyside6` on Arch).
Nothing else: Qt decodes the frames. Without PySide6, `--snapshot x.jpg`,
`--stats` and `ztos touch` still work with the standard library alone.

The viewer connects out to the device, so a firewall on this computer doesn't
get in the way.

## Why JPEG, not H.264

- **The viewer's decoder:** EL 10 and Fedora ship no H.264 decoder without extra
  repositories. `libavcodec-free` and `noopenh264` don't decode it, and
  `gst-inspect-1.0` finds no `avdec_h264` or `openh264dec`. Qt reads JPEG on any
  host.
- **The link:** USB 2 has plenty of room for it (see `ROADMAP.md` 6.1 for the
  measurements).
- **The device:** there's no hardware encoder (no `/dev/video*`). libjpeg-turbo
  costs less CPU than x264 at this size and adds no reference-frame delay.

The wire format isn't scrcpy's. Its client needs adb and an Android server
anyway, and its touch message carries fields the control pipe has no use for.

## Safety

- **Listening:** only on a wired interface. The default is `172.16.42.1`, or
  QEMU's `10.0.2.15`. `--bind` picks another, but Wi-Fi is refused. The socket is
  tied to its interface with `SO_BINDTODEVICE`, because a socket bound only to an
  address still accepts packets for it from Wi-Fi.
- **Token:** a client needs the random token, made fresh at each start and
  written mode 0600.
- **Control pipe:** the pipe also takes commands like `launch`. Only `down`,
  `move`, `up`, `cancel` and `key` lines are written to it, built from numbers.
  No client text reaches it.
- **Held input:** when a client hangs up, goes silent for 5 s (a pulled cable
  sends no FIN), is replaced, or ztmirror gets SIGTERM, every held finger is
  cancelled and every held key released. Only SIGKILL or a crash can leave a
  finger down.
- **Off by default:** the service has no `[Install]` section, and ztmirror exits
  after 10 minutes with no client.

One client at a time; the newest replaces the last. The window then offers to
take the device back, rather than fighting a `ztos touch` for it.

## Protocol

Numbers are big-endian. The client opens with one text line:

    ZTMIRROR 1 TOKEN scale=2 quality=70 fps=30\n

`scale` is 1, 2 or 4 (1/N of the pixels). After that, the client sends 8-byte
messages, `type id a b reserved`, as u8 u8 u16 u16 u16:

| type | message | id | a, b |
|---|---|---|---|
| 1 | down | finger, 0 to 9 | x, y: 0 to 65535 across the screen |
| 2 | move | finger | x, y |
| 3 | up | finger | |
| 4 | cancel | finger | |
| 5 | key | 0 power, 1 volume up, 2 volume down, 3 home, 4 back | a: 1 down, 0 up |
| 6 | ping | | echoed back; send one every second or so, or be taken for gone |

Each message goes to the shell as it arrives, never merged, because the shell
times a fling by when moves reach it. Send them as they happen, with
`TCP_NODELAY`.

The server sends `type (u8) length (u32) payload`:

| type | message | payload |
|---|---|---|
| 1 | frame | seq u32, device time µs u64, encode µs u32, w u16, h u16, JPEG |
| 2 | status | JSON: `points`, `scale`, `pixels`, `stream`, `screen` ("on"/"off"), `compositor`, `fps`, `error` |
| 3 | error | text; the connection then closes (a wrong token, or another client connected) |
| 4 | pong | a u16, b u16, device time µs u64 |

Status comes first and then every second, so silence means the link is gone, not
a still screen. Frames come only when the screen changes, and not at all while
it's off.

## The shell's side

`$XDG_RUNTIME_DIR/ztmetro.ctl`, added for this: `key CODE down|up` with the evdev
code of power (116), volume up (115), volume down (114), home (172) or back (158),
through the same logic as the real keys.

`$XDG_RUNTIME_DIR/ztmetro.state`, replaced whenever the screen changes:

    points 360 740
    scale 4.00
    pixels 1440 2960
    screen on

## Testing

`ztmetro/tests/ztmirror-input.py mirror/ztmirror` checks the input path with a
FIFO in place of the shell: no shell or compositor needed.
