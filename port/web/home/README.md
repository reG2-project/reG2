# reG2 landing page

The page at the root of the GitHub Pages site. Plain HTML and CSS, no build step; the only JavaScript is the classic and enhanced slider's few lines.
The Web workflow publishes it like this:

| Site path | Source |
|---|---|
| `/` | `port/web/home/` (this folder) |
| `/ds.css` | `port/web/ds.css` |
| `/play/` | the game page, `port/web/site/` plus the WebAssembly build |

`index.html` links `ds.css` as a sibling, so open it through that layout, not straight from this folder.

## Preview

```sh
P=/tmp/reg2-site
rm -rf "$P" && mkdir -p "$P/play"
cp -R port/web/home/. "$P/" && cp port/web/ds.css "$P/"
cp -R port/web/site/. "$P/play/"   # optional: the game page, or leave /play/ empty
python3 -m http.server -d "$P" 8000
```

Then open <http://localhost:8000/>. The site is dark only.

## Files

- `img/`: copies of `docs/images/*` (the reG2 logo for dark backgrounds, screenshots) and `port/resources/reg2_256.png`, so the
  page works when only this folder is published. `pc_compare_classic.webp` and `pc_compare_enhanced.webp` are the
  two frames of `docs/images/pc_compare_graphics.png` (796x448 each, without the label bar). Git stores identical files once, so the copies cost nothing
  in the repository; replace them when the originals change. Only screenshots from the repository, and none
  that show the GTA2 or Rockstar logos large.
- `fonts/`: Michroma and Archivo (variable), latin subsets as woff2, both under the SIL Open Font License
  (licences next to them), from the Google Fonts repository. `ds.css` loads them from `fonts/` beside itself,
  which is this folder once published.
- `home.css`: this page's layout only.

## Design (`ds.css`)

Shared by this page and the game page, dark only. The look follows the game's own frontend: a darkened city
behind the menu, wide tracked capitals, white items and the selected one in red. One accent, red, for the selected or main action and the
section bars. The screenshots carry the colour.

- Colours: `--bg`, `--bg-2` (raised surfaces), `--line`, `--text`, `--muted`, `--red` (the selection and main
  action), `--red-ink` (text on red), `--amber` (warnings), `--ok`, `--focus`.
- Type: `--font-display` (Michroma, close to the square wide lettering of the game's menus and logo, for
  headings, labels and menu paths), `--font-body` (Archivo), `--font-mono` (system monospace, for code),
  `--track` (letter spacing of the capitals); sizes `--step--1` to `--step-3`.
- Space: `--space-1` to `--space-7`, `--measure`, `--page-max`, `--gutter`.
- Components: `.caps` (tracked capitals), `.menu` (a vertical menu, `.sel` on the selected item; the pointer
  moves the selection), `.nocase` (keeps macOS or gta2_re in `.caps`), `.muted`, `.small`, `.skip`.

Two touches of the era, kept faint: a fine grain over the background (an SVG noise tile) and a bold bar over each
section title, in the red of the game's menus. Both in `home.css`, not on the game page.

No Rockstar or GTA2 logos, fonts or artwork as branding; no gradients, glows or other decorative effects.
