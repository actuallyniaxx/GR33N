# GR33N — XMB entry resources

Anything you drop into `pkgfiles/` under these exact names is picked up
automatically, both by `make pkg` and by `sh deps/install-xmb.sh`. If a file
isn't there it simply isn't used: nothing is mandatory except the icon.

## What goes in, and at what size

| File | Size | What it is |
|---|---|---|
| `ICON0.PNG` | 320 × 176 | The icon in the list. The one you really do want: without it, some firmwares don't draw the entry at all. |
| `PIC1.PNG` | 1920 × 1080 | The full-screen background shown while the entry is selected. It's what makes it look like a real application rather than homebrew. |
| `PIC0.PNG` | 1000 × 560 | An image overlaid on top of the background. Almost nobody uses it; it's here in case you want to. |
| `SND0.AT3` | — | A short jingle when the entry is selected. ATRAC3 format — not MP3, not WAV. |
| `ICON1.PAM` | — | Animated icon. A proprietary Sony format and a pain to generate. Not worth it. |

`sfo.xml` lives in the **repository root**, not in `pkgfiles/`. That's where
`CATEGORY` lives, which decides what column of the XMB GR33N appears in —
currently `CB`, meaning Network.

> [!WARNING]
> Only put resources the XMB will read inside `pkgfiles/`. The PSL1GHT rule
> copies that directory **whole** into the package, so a stray `sfo.xml`, a
> README or a backup file ends up installed on the console.

## The PNGs: the sizes are exact

The XMB does not scale these images. If `ICON0.PNG` isn't exactly 320 × 176,
what happens depends on the firmware: sometimes it crops, sometimes it
stretches, sometimes it just doesn't draw. It's a measurement, not an upper
bound.

From the command line, with ImageMagick:

```sh
magick whatever.png -resize 320x176!  ICON0.PNG
magick background.jpg -resize 1920x1080! PIC1.PNG
```

The exclamation mark matters: it forces the size even if that distorts the
image. Without it ImageMagick preserves the aspect ratio and hands you a
320 × 180 that won't do.

## After changing something

```sh
make pkg                    # -> gr33n.pkg, to install
sh deps/install-xmb.sh      # -> xmb/GR33N0PS3/, to copy across by hand
```

> [!TIP]
> The XMB caches icons stubbornly. If you change `ICON0.PNG` and still see
> the old one, reinstall the application or restart the console — it isn't
> that your file was ignored.
