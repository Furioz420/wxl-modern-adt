# Modern ADT

> The client opens one terrain file per tile and expects everything to be inside it. Modern terrain
> is not built that way any more: a tile is three files that only make sense together.

This module reads all three and fills the client's terrain runtime directly. **Nothing is written to
disk, nothing is repacked, and no native map is touched.** A tile that was already whole is served
exactly as it is.

| The tile is split across | Which carries |
|---|---|
| the root file | header, heightmap, normals, vertex colours, liquid |
| the texture file | layers, alpha maps, shadow |
| the object file | doodad and object placements, name tables |

---

## It decides by looking, not by asking

There is no version number to trust. The module keys off what the data actually contains, so one path
serves any split source rather than a list of known formats that goes out of date.

Detection is per map and cached: the first tile requested answers the question for the whole map.

---

## The differences it has to reconcile

A tile authored for a newer version assumes an engine that moved on. Everything below is a place
where the client would otherwise read something it cannot use.

### Alpha maps

Re-packed to the compact form, so tiles read through this module sit next to the map's own tiles
without either looking wrong.

### Holes

Authored at high resolution, folded down to the coarse mask the client reads. The original is kept
aside rather than discarded, so nothing is lost to the tile that carries it.

### Placements

Flags are masked to the bits the client understands, and the per-instance scale is preserved for the
runtime that uses it.

### Texture layers

Beyond the client's own cap they are clamped. The tail belongs to the multi-pass terrain work and
ships with it, never on its own.

### Assets referenced by number

Modern tiles name their doodads and objects by id rather than by path. Those are resolved and the
name tables rebuilt.

An object whose name cannot be resolved is **left out rather than spawned**. That is not tidiness: a
doodad with a missing name degrades into a placeholder cube, while a map object with one takes the
client down. The two failures are not equivalent and are not treated as if they were.

### Liquid

Water layers are normalised into what the stock builder expects. A layer whose type the live table
cannot serve is degraded rather than dropped, and a layer whose data points outside its own block is
degraded too, instead of letting the builder read past the end of it.

---

## Height blending

Terrain layers can blend by height rather than by a flat alpha ramp, which is what stops a rock
texture from fading into grass like a watercolour. Layers carrying height data use it; layers
without it fall back to the plain blend.

Two settings, in the module's own configuration file next to the DLL:

| Setting | Does |
|---|---|
| `WXL_ADT_HEIGHT_BLEND` | master switch |
| `WXL_ADT_HEIGHT_SHARPNESS` | how hard the transition bites. `0` is a plain scale and offset blend |

The low-detail world map that draws the distance is read directly too, so the far horizon matches the
ground you are standing on.

---

## What it tells you

Session counters are readable from Lua: how many maps were detected as split, how many tiles loaded,
how many are resident right now, how many placements resolved, how many liquid layers were degraded
and why.

Every tile can also be inspected individually, down to which of its optional blocks were parked.

## Interfaces

- Reads `wxl.fdid`, to turn an asset id into a path. Without it, tiles that name their assets
  directly still load; tiles that reference them by id will not find them.
- Reads `wxl.loadpool` for streaming.

## Requirements

WarcraftXL on a 3.3.5a client, build **12340**. The module refuses to load against anything else
rather than guessing, and says so in the log.
