# CyberFoil
[![Latest version](https://img.shields.io/github/v/release/luketanti/CyberFoil?label=version)](https://github.com/luketanti/CyberFoil/releases/latest)
![GitHub all releases downloads](https://img.shields.io/github/downloads/luketanti/CyberFoil/total)
![GitHub latest release downloads](https://img.shields.io/github/downloads/luketanti/CyberFoil/latest/total)
[![Discord](https://img.shields.io/badge/Discord-Join%20Server-5865F2?logo=discord&logoColor=white)](https://discord.gg/gGy7hWxJeP)


## Features
- Simple homebrew file installer
- Works best with [AeroFoil](https://github.com/luketanti/AeroFoil)
- Save Data Backup manager: browse console/server saves, upload with notes, download specific own save data versions, and delete save data files from server

## Save Sync (Only works with compatible [AeroFoil](https://github.com/luketanti/AeroFoil))
- Available when the account has backup access.
- Shows local console saves and remote server save data backups in a dedicated `Saves` section.
- Upload supports version notes.
- Download/delete of remote save data backups supports per-version selection with an in-page selector layout.

## Cheats (Currently only works with [AeroFoil](https://github.com/luketanti/AeroFoil))
- Browse cheats published by a compatible [AeroFoil](https://github.com/luketanti/AeroFoil) server in the dedicated `Cheats` section.
- Download and install individual cheats, or install all available cheats at once.
- Cheats are installed to `atmosphere/contents/<title-id>/cheats/<build-id>.txt`.
- Manage local cheats: view, create, edit, and delete cheat files; upload edited cheats to [AeroFoil](https://github.com/luketanti/AeroFoil) when permitted.

## Offline Metadata/Icons
- CyberFoil can use local title metadata and local icons from `sdmc:/switch/CyberFoil/offline_db/`.
- Optional `custom_titles.pack` and `custom_icons.pack` in that directory add entries alongside the default `titles.pack` and `icons.pack`. When a title ID appears in both, the custom title metadata or icon takes priority. Either custom pack can be used independently and must use the same binary format as its default pack. Offline database updates preserve these files; restart CyberFoil after manually replacing them.

### Creating custom packs

Use Python 3.8 or newer. Install Pillow with WebP support for image conversion:

```sh
python -m pip install "Pillow>=9.1"
python tools/create_custom_packs.py --titles my_titles.json --icons-dir my_icons --output custom_packs
```

Either `--titles` or `--icons-dir` can be omitted. The tool writes `custom_titles.pack` when metadata is supplied and `custom_icons.pack` when images are supplied. Copy the generated packs into `switch/CyberFoil/offline_db/` on the SD card, then restart CyberFoil. Existing output packs of the same kind are replaced; an output pack for an omitted kind is left untouched.

Title information is a UTF-8 JSON object keyed by 16-digit hexadecimal base title ID, for example:

```json
{
  "0100000000001000": {
    "name": "My Custom Title",
    "publisher": "My Publisher",
    "intro": "Short introduction",
    "description": "Full description",
    "size": 1048576,
    "version": 65536,
    "releaseDate": 20261013,
    "isDemo": false,
    "icon": "images/my-title.png"
  }
}
```

All fields are optional, but each entry needs metadata or an `icon`. `size` is in bytes, `version` is the numeric title version, `releaseDate` uses the numeric `YYYYMMDD` format, and `isDemo` is a JSON boolean. A custom title entry replaces the entire default entry for that ID, so include every metadata field you want to keep.

An `icon` path is relative to the JSON file (absolute paths also work). Alternatively, place images named after title IDs, such as `0100000000001000.png`, directly inside `--icons-dir`. Explicit JSON icon paths take priority over images in that directory. Supported input extensions are PNG, JPG/JPEG, WebP, BMP, and TIF/TIFF; images must match their file extension. Icons are processed with the same settings as Offline-DB-Maker: EXIF orientation is applied, RGBA transparency is composited onto white, other color modes are converted to RGB, and images are center-cropped and resized to **128×128** with Lanczos, then encoded as **WebP, quality 80, method 6**. Original image files are left untouched. Converted icons must be no larger than 16 MiB each. Duplicate IDs, invalid fields, missing or corrupt images, and pack size limits are checked before output is written. Titles-only generation does not require Pillow.

Run the pack format regression tests with `python -m unittest discover -s tests -p custom_packs_tests.py`.

## External themes

Copy one or more theme JSON files into `sdmc:/switch/CyberFoil/theme/`, then open **Settings > System > Theme** to select one. Use **A** or tap a theme to save and apply it immediately; **B** returns to settings. The picker stays open and the checked row shows the selection. **Default** restores the built-in appearance without removing any theme files. After editing a theme or its images, select it again to reload them. Changes wait until an active installation finishes before layouts are recreated.

An **Applying theme…** screen shows an animated spinner and progress bar during the change. Layouts are recreated one per frame, then the theme picker returns automatically.

The picker lists valid, enabled `.json` files directly in `theme/`, plus `theme.json` inside each immediate subfolder. For themes with their own images, use a folder such as `theme/my-theme/theme.json` and put its assets alongside that JSON file. Image paths are relative to the selected JSON file's folder. Disabled themes, malformed JSON, and unsupported versions are omitted from the picker. A missing or invalid saved theme falls back to the built-in appearance at startup.

The selection is stored as `themeFile` in `config.json`: a filename such as `violet-dusk.json`, a relative path such as `my-theme/theme.json`, or an empty string for **Default**. Existing installations still load `theme/theme.json` until a selection is made.

Example palettes are available in `examples/theme/`:

- [Midnight Blue](examples/theme/midnight-blue.json): navy panels with blue accents.
- [OLED Mint](examples/theme/oled-mint.json): black background with mint accents.
- [Forest Green](examples/theme/forest-green.json): dark green panels with leaf-green accents.
- [Violet Dusk](examples/theme/violet-dusk.json): purple panels with lavender accents.
- [Ember Amber](examples/theme/ember-amber.json): warm brown panels with amber accents.
- [Crimson Night](examples/theme/crimson-night.json): deep red panels with crimson accents and bright text.

Copy any or all of these files into `switch/CyberFoil/theme/` on the SD card, keeping their filenames. Select one in **Settings > System > Theme** to apply it immediately. These examples need no additional images and use the built-in logo and icons.

The schema is specific to CyberFoil; existing Tinfoil theme JSON files need adapting. Version 1 supports shared colors, a background image, a logo/banner, and individual built-in UI image replacements across the main menu, Remote, local file browsers, installation screen, and settings. It keeps the existing geometry and fonts. Per-element border widths, border colors, and the framework's modal dialogs/system keyboard are not customized by this version.

```json
{
  "version": 1,
  "enabled": true,
  "name": "My Theme",
  "backgroundDim": 0.3,
  "colors": {
    "background": "#101827",
    "text": "#EAF2FFFF",
    "selected_text": "#FFFFFFFF",
    "accent": "#56A8FFFF",
    "highlight": "#245F9DCC"
  },
  "images": {
    "background": "background.png",
    "logo": "banner.png",
    "icons/settings.png": "icons/my-settings.png",
    "icons/folder.png": "icons/my-folder.png"
  }
}
```

All fields except the JSON object itself are optional. Colors use `#RGB`, `#RGBA`, `#RRGGBB` or `#RRGGBBAA`, with alpha last; the `#` is optional, so CSS colours and Tinfoil's bare `RRGGBBAA` values both work. Colors without alpha are opaque. Supported color names are:

| Name | Usage |
| --- | --- |
| `background` | Layout background; suppresses the default background image when no custom image is supplied |
| `header`, `footer` | Top and bottom bars |
| `panel` | Information bars, tiles, queue cards, and overlays |
| `text` | Primary labels and menu text |
| `selected_text` | Focused menu items, main-grid labels, Remote focus title/marquee, and selected queue titles |
| `muted_text` | Secondary installation queue labels |
| `accent` | Progress bars and accents; also tints highlights and scrollbars unless their own colors are supplied |
| `highlight` | Focus backgrounds and selected tiles |
| `scrollbar` | Menu scrollbar |
| `progress_track` | Installation progress bar track |

Shared `panel` and `accent` tints retain the original element's opacity, multiplied by the theme color's alpha. Explicit `header`, `footer`, `highlight`, and `scrollbar` values use the supplied alpha directly. Battery/network status and completion/error colors keep their existing meaning.

Image paths are relative to the selected JSON file's directory; use forward slashes, and keep assets inside that directory. `background` and `logo` are aliases for `background.jpg` and `logo.png`. Other image keys match paths under `romfs/images/`, for example `icons/micro-sd.png`, `icons/remote.png`, `icons/usb-install.png`, `icons/checkbox-blank-outline.png`, and `icons/check-box-outline.png`. Override both checkbox images when changing their design. An empty path (`"icons/settings.png": ""`) hides that image, as it does for the background; for example, `"awoos/5bbdbcf9a5625cd307c9e9bc360d78bd.png": ""` removes the main-menu mascot. A theme's own mascot, or a hidden one, takes priority over `awoo_main.png` and `awoo_inst.png` in `sdmc:/switch/CyberFoil/`. Game cover art continues to come from the title database or Remote.

Use a **1280×720** background and a **480×93** banner (the banner is fitted into that existing space). A background of any other size is scaled to cover the screen, centred, so a 1920×1080 wallpaper shows whole. `"backgroundDim": 0.35` darkens the theme's background image (0 to 1) so text stays readable over busy art, without editing the image. PNG, JPEG, and WebP work through the bundled image decoder. Assets must be valid, no larger than 16 MiB each, and no larger than 4096 pixels on either side. An empty background path (`"background": ""`) disables background images. Custom background settings override OLED mode's default background; omitted settings keep the current OLED/default appearance and legacy `background.png` behavior.

Missing or corrupt images and invalid individual colors fall back to the existing assets/colors. Malformed JSON or an unsupported schema version restores the default theme. The JSON file is limited to 256 KiB. Removing the theme never removes user assets.

## Remote Configuration
- For LAN remote, in Settings > Remote, choose **Discover LAN Servers**. Scans take about three seconds; B cancels and X scans again. Results show name, LAN address/HTTP port, version, and public/private status. Select a result, then use the normal Remote form and **Save Remote** to save/apply it. Manual entry remains available and discovery does not overwrite a saved URL on its own.
- [AeroFoil](https://github.com/luketanti/AeroFoil)'s **LAN discovery** setting must be enabled, and UDP **8465** must be allowed through the server firewall. Wi-Fi client isolation and separate subnets can prevent discovery. Docker must deliver LAN broadcasts to the container; host networking works on supported platforms (publishing a UDP port alone may not deliver broadcast traffic).
- Each Remote can have a **LAN URL** and a **Remote URL**. Enter only the IP or hostname and optional `:port`, e.g. `192.168.1.100:8465` and `remote.example.com:443`; schemes are supplied automatically. **LAN protocol** has an HTTP/HTTPS dropdown. **Remote URL** always uses HTTPS. Omitted ports use HTTP 80 or HTTPS 443. Either address may be left empty, and existing optional paths remain supported. Username/password are shared between both addresses.
- **Connection** can be **Auto (LAN first)**, **LAN only**, or **WAN only**. Auto checks LAN connectivity with a roughly two-second timeout, then uses WAN if LAN is unreachable. With one address configured, it uses that address directly. Authentication and catalogue errors do not trigger fallback. The selected address stays fixed during browsing/downloads; reopening Remote after the console's IP changes, or explicitly refreshing the catalogue, rechecks connectivity.
- Discovery fills LAN URL from the reply source IPv4 address and advertised HTTP port. A valid optional `remote` value can suggest the WAN URL for review; it never replaces an existing WAN URL. A configured host without a scheme is suggested as HTTPS; explicit HTTP remote metadata is ignored. Saved discovery UUIDs let rediscovery update the same profile; manually entered profiles with the same LAN URL can also be reused. Discovery supplies no credentials and preserves the profile's credentials, Remote mode, and User-Agent settings. Private servers still need the normal username/password configuration.
- Existing profiles retain their original URL and credentials. Legacy public HTTPS addresses go into Remote URL; private IPs and local hostnames remain in LAN URL. Importing a standalone URL from an old config runs once; modern config reloads and runtime WAN selection do not create additional Remote entries. Remote JSON files additionally store `lanUrl`, `wanUrl`, `connection` (`auto`/`lan`/`wan`), and `uid`; the previous protocol/host/port/path fields remain for compatibility. `remoteProfileFile` in `config.json` identifies the active saved profile regardless of which endpoint is used at runtime.
- `sdmc:/switch/CyberFoil/config.json` supports optional `httpUserAgentMode` and `httpUserAgent`.
- `httpUserAgentMode` can be `default`, `chrome`, `safari`, `firefox`, `tinfoil`, or `custom`.
- `default` uses `cyberfoil`.
- `tinfoil` sends an empty User-Agent value.
- Preset/custom modes are applied across network requests (downloads, Remote, and save sync).
- `httpUserAgent` is used when `httpUserAgentMode` is `custom`.

## Installation controls

- The USB SSD/HDD browser hides dot-prefixed files and folders by default, including macOS `._` sidecar files and `.Trashes`, plus Windows metadata names `$RECYCLE.BIN`, `System Volume Information`, `desktop.ini`, and `Thumbs.db` (matched case-insensitively). Press **−** or tap **Show hidden / Hide hidden** in the bottom bar to toggle visibility. Show hidden reveals filtered folders and installable files; `.ini` and `.db` files remain excluded by the installable-file extension filter. The preference is saved and is also available in **Settings > General > Show hidden files on USB drives**. Hidden entries are excluded from Select All and new install selections while hiding is enabled.
- **B** immediately requests cancellation of the active title and stops the remaining batch. The screen stays responsive while rollback finishes. New content and placeholders from that title are removed; previously installed content and completed titles are retained. Title records are published after content transfer completes.
- **X** blanks the screen and turns off the handheld backlight while installation continues. Any button or touch wakes the screen; the wake gesture does not activate a control. The screen also wakes when the batch finishes or fails.
- The installation screen shows an active-title card and six numbered pending rows with cached artwork, type/size details, a selection highlight, and waiting/completed counts. **Up/Down** selects a row, **Left/Right** pages six rows, and a touch selects a row. **A** removes the selected pending title.
- **Y Add titles** opens the normal shop browser during Remote installation. Search, sections, list/grid view, and multi-selection remain available. Select titles with **A**, then press **+ Add to queue** to append them to the existing storage target and return to the queue; **B Queue** returns without adding. Active, queued, and completed titles cannot be added twice. The session stays open if the queue drains while choosing more titles. SD/USB drive installs return to the current folder's file browser. PC USB/network batches remain limited to the supplied batch; MTP transfers remain host-controlled.
- Batch installs run on a worker while the UI polls input and renders independently of transfer progress. Holding a direction repeats queue navigation. Progress updates are coalesced; all dialogs and texture changes execute on the UI thread.
- Queue and UI-dispatch regression tests are in `tests/install_queue_tests.cpp` and `tests/ui_dispatcher_tests.cpp` and run with `make -C tests test` on a POSIX host. Device checks should cover short button taps and held navigation during slow HTTP requests and local writes, canceling an NSP/XCI midway, preserving existing content, editing the queue during transfer, and waking the screen without canceling.

## Release Package Layout
- Build output: `cyberfoil.nro`
- SD layout in release zip: `switch/CyberFoil/cyberfoil.nro`

## Note
- Uses [XorTroll's Plutonium](https://github.com/XorTroll/Plutonium) for a pretty graphical interface
- Forked from [Awoo Installer](https://github.com/Huntereb/Awoo-Installer)




