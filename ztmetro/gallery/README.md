# ztGallery

The phone's photos, in the Windows Phone Photos look: a pivot of **photos**
(everything, newest first, four across) and **albums** (one per folder), and a
full screen viewer with swipe, pinch, pan, double tap and delete. Sharing and
editing are not here.

Build it on the phone with `make gallery/ztgallery` from `ztmetro/`. It is part of
`make` and `make install`, so the installer's image has it. `make check-gallery` runs
the tests of its parsing and arithmetic (`tests/gallery.c`, no display or libraries).

    ztgallery [PATH]                the gallery, or PATH open in the viewer
    ztgallery --pick                choose a photo (below)
    ztgallery --render PAGE FILE    a page (photos, albums) as a PNG, no display

## Choosing a photo, for other programs

This is how another program gets a photo from the user. It is what 7.8's "from
gallery" is for. No other app in the tree has a picker, so this sets the convention:

    path=$(ztgallery --pick) && use "$path"

| Call | The answer |
| --- | --- |
| `ztgallery --pick` | the path and a newline on stdout |
| `ztgallery --pick-null` | the path and a NUL on stdout, for paths with newlines in them |
| `ztgallery --pick-out FILE` | the path and a newline in `FILE`, written to a temporary file and renamed, and nothing on stdout: for a caller with no pipe to read, such as a command the shell launches |

| Exit status | Meaning |
| --- | --- |
| 0 | chosen; the answer is there |
| 1 | the user backed out, or the window went away; there is no answer |
| 2 | the command line was wrong, or the answer could not be written |

- The path is absolute and exactly the file's bytes. It may hold spaces, quotes, `$`,
  or anything else a file name can, so pass it on as one argument and never through a
  shell without quoting it. `--pick-null` is the only form that survives a newline in
  a name when read with `$(...)` or line by line.
- Nothing else ever reaches stdout: from the start of a pick it points at stderr, and
  only the answer goes to the real one.
- The grid is the normal one, with **photos** and **albums**, under the caption CHOOSE A
  PHOTO. Tapping a photo chooses it at once, as Windows Phone's photo chooser does.
  Back cancels. A file that could not be read can't be chosen. There is no viewer and
  no delete while picking.
- The picker runs as its own process with its own window, beside any gallery that is
  already open. The caller waits for it to exit. From C, use `g_spawn_sync` or
  `fork`/`execvp` with the arguments as an array and a pipe on stdout; `popen` runs a
  shell, which is fine here only because the command line is fixed.

For 7.8, a shell that launches through `launch()` (`/bin/sh -c "exec COMMAND"`, stdout
inherited) has no pipe to read, so `--pick-out` under `$XDG_RUNTIME_DIR` suits it: wait
for the child to exit, then read the file if the status is 0. Remember that `launch` is
also where the shell's "return to it if it runs" logic lives, and that it knows nothing
of a window whose only purpose is an answer.

## Where photos come from

Folders are read on a thread, and looked into up to 8 levels down:

- `~/Pictures`, or wherever `xdg-user-dir PICTURES` says. Camera saves in
  `~/Pictures/ztCamera`, which shows as the album "camera roll".
- `~/DCIM`, `~/Screenshots`, `~/Photos`, `~/Android/DCIM`, `~/Android/Pictures`.
- `~/Downloads`, its top level only.

Hidden files and folders, folders named `thumbs` or `thumbnails`, and symbolic links are
skipped; each real folder is read once. At most 50 000 photos and 4096 folders. A change
in any folder read (up to 512, by inotify) brings a new scan 0.6 s later, and what has
not changed keeps its thumbnail.

JPEG, PNG and GIF (its first frame), and WebP when gdk-pixbuf has a loader for it. The
order is the file's modification time, not the date in its EXIF data, which would mean
opening every file.

## How it stays smooth and small

Decoding and folder reading never happen on the main thread. A scan thread, two thumbnail
workers and a viewer worker hand finished surfaces over a queue, and an eventfd wakes the
main loop.

- **The grid** is a `zmc_list` whose rows are only grey squares. Each thumbnail is a
  264 px texture drawn over its square from `.overlay`, as Settings' wallpaper grid does,
  so an arriving thumbnail doesn't redraw the page. 72 are held at once. While a pivot
  or a page turn moves a picture of the page, the same thumbnails are painted into it.
- **The thumbnail cache** is `~/.cache/ztgallery/thumbs/`: a file named by the SHA-256 of
  the photo's path, time and size, so any file name is safe and a changed photo makes a
  new one. A photo that can't be read leaves a `.bad` marker, and costs one attempt until
  it changes. Above 256 MB the oldest go, down to 200 MB.
- **The viewer** holds the photo in front and the two beside it, decoded to fit the
  screen, as bands of 256 rows in textures, uploaded a few per frame. Their pixels are
  freed once the GPU has them. Zooming past 1.2x loads the photo in front at up to 4096 px
  on its long side, replacing the screen-sized one until it zooms back out.
- **libjpeg.** On this system gdk-pixbuf 2.44 decodes through glycin: a sandboxed process
  that decodes the whole image. That took about 700 ms for a 12 MP JPEG thumbnail. So JPEG
  goes straight to libjpeg-turbo, which decodes at n/8 of the size. It's optional at build
  time (`pkg-config libjpeg`, in `os/packages/common`). Everything else, and a JPEG libjpeg
  refuses, goes through gdk-pixbuf, so glycin's sandbox still covers those formats.
- **Limits.** PNG, GIF and WebP above 100 MP aren't decoded, as they'd be decoded whole
  first; JPEG above 500 MP isn't either. They show as unreadable.
- **Unreadable files** show a crossed square in the grid and a message in the viewer, where
  they can still be deleted. libjpeg's errors come back through `setjmp`; the header
  parsing in `gallery-logic.h` is tested against truncated and random bytes.

Delete is `unlink` on the path, after a confirmation, with no shell involved. It can't be
undone: there's no recycle bin on the phone.

## Touch and the core

`zmc_app.touch` used to get a single finger. A pinch needs two, so the core gained an
opt-in `zmc_app.multitouch`: while the app holds a finger, further fingers come to `.touch`
too, and the territory stays the app's until the last one lifts. Other apps are unaffected.

The shell keeps the left edge: a finger that comes down within 18 points of it and
swipes right closes the app, the viewer included, so a pinch can't start there.

The viewer draws in `.overlay`, and needs the GPU. So does the grid's thumbnails, like
Settings' wallpaper grid. Without one the grid stays grey and a tap does nothing.
Under the DotMatrix style photos are still plain photos on the GPU, as in Camera's roll;
the pictures of the page used during a page turn are dots.
