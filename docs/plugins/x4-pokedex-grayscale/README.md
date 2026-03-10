# X4 Grayscale Pokedex Wallpaper (Companion Plugin)

This companion plugin vendors and constrains:
- Upstream: https://github.com/m86-tech/BOOX-Pokedex-Wallpaper-Generator
- Commit: `2cadbb360a42c460c15dd6461185a3c98df7e40c`

CrossPoint-specific constraints in this variant:
- Locked resolution: `480 x 800` (XTEink X4)
- Grayscale sprite rendering is always enabled
- Single-image `.bmp` export is always used
- Browser-first workflow for generating assets outside the device

This page is a supported primary workflow for users who generate wallpapers on the web and then copy them to the SD card.
It is not just a fallback for the device-hosted `/plugins/pokedex` page.

Open `index.html` in a browser and generate wallpapers for the `/sleep/pokedex/` folder.
The generated `.bmp` files should be copied to `sleep/pokedex` on the SD card.

The companion page can also export a stripped `pokemon_cache.json` for the device-hosted
`/plugins/pokedex` page. To bake that cache into firmware builds:

```bash
python scripts/inject_pokemon_cache.py /path/to/pokemon_cache.json
pio run
```

Use `python scripts/inject_pokemon_cache.py --clear` to remove the local baked cache sidecar.

## Attribution
Original project is MIT licensed by m86-tech.
See `UPSTREAM_README.md` for original upstream documentation.
