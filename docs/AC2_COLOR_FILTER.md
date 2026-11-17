# AC2: Remove color filter

The launcher's **Remove color filter** (Assassin's Creed II only) switches off
the color filter the game puts over the picture, in every world. It is the
effect of the PC mod *AC2's True Colors* (Nexus Mods, Assassin's Creed II, mod
179), which we owe the finding of what to change; nothing of the mod's files is
used or kept, and the game's own files are not changed.

`SCIMITAR_NO_COLOR_FILTER=1` does the same from the environment.

## What the mod changes

Each world's archive (`DataPC_<World>.forge`, `DataMAC_<World>.forge`) holds a
`WorldAmbiance` record for the world's default ambiance (sky, fog, light and the
filter): 1687 bytes, laid out the same way in every world. The mod's 21 files are
those records with 11 bytes changed (the same 11 in each, checked against the
Mac release's Florence record: they are the only difference):

| Offset | Size | Mac release (Florence) | Mod |
|---|---|---|---|
| 97, 158, 183 | 1 byte each | 1 (the byte before a group of three floats: an on/off) | 0 |
| 1423 | 2 floats | 0.15, 0.35 | 0, 0 |

The Windows data's object references inside the record are the Mac's (the
records match byte for byte apart from the 11), so the mod's values apply to the
Mac archives as they are.

## How it is done

`games/ac2/color_filter.cpp` replaces `WorldAmbiance::FastLoad` in the runtime's
function table, like `pc_data.cpp`'s hooks, and patches the record waiting in the
serializer's buffer before the original reads it (`color_filter_patch.h`). The
record is recognised by 16 four-byte values that every world has at the same
offsets (15 must match), not by where it is, so a record of another layout is
left alone. The read position can be a few bytes into
the record already when `FastLoad` is called (a run's log showed 1677 of the
1687 bytes left), so the patch also takes a record whose first bytes, up to 32,
were read: it uses the anchors still in view (at least 12, at most one wrong).
When nothing fits, the log gives the closest fit and the first 16 bytes. `SCIMITAR_AMBIANCE_SYMBOL=<mangled name>` names the function if the
built-in guess for its symbol is not the game's.

`tests/ac2/color_filter_test.cpp` runs the patch on look-alike records made from
the fingerprint (no game data).

## What is checked, and what is not

- Checked: on the Mac release's Florence record the patch gives exactly the mod's
  Florence file; all 21 of the mod's files are recognised.
- Checked by the test: a record whose first 0 to 32 bytes were read already.
- Not checked: that the 10 missing bytes in that log are a header of the record
  (the next log says: it names the closest fit); the other 20 worlds' Mac records (`tools/find_ambiance.py` finds a
  world's record in its archive), and that the picture looks as it does with the
  mod.
