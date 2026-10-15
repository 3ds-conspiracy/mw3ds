# Plugins and translated game data

Rules read from OpenMW's `components/esm3/loadcell.cpp` (adjustRefNum, getNextRef, loadNameAndData, loadCell),
`components/esm3/infoorder.hpp`, `components/esm3/loaddial.cpp`, `apps/openmw/mwworld/store.cpp` (cells, moved
references, dialogue, LTEX, LAND), `mwworld/cellstore.cpp` (loadRefs, loadRef, CellRefList::load),
`mwdialogue/keywordsearch.cpp` (parseHyperText), `components/translation/translation.cpp` and the places OpenMW
shows a cell name (`#{sCell=...}`). In our words. Our side is `tools/convert/mwfiles.py` (read_game, load_db) and
the engine's `GameData::gmstLabel / cellLabel / topicStandardForm / topicKeyword`, `dialogueHyperText`. The check
is `tools/test/plugincheck.py`, against the official plugins in the player's own Data Files.

Scope: plugins whose masters are `Morrowind.esm` (and plugins before them). A plugin needing `Tribunal.esm` or
`Bloodmoon.esm` is refused at convert time. MWSE, MGE XE and OpenMW Lua mods are out.

## 1. Load order

`MW3DS_PLUGINS` lists the plugins (`;`-separated, names in Data Files), in load order, after `Morrowind.esm`. Each
plugin's `MAST` entries must name files loaded before it. OpenMW takes its content list from `openmw.cfg`; we take
it from `.env` so a conversion doesn't depend on a launcher. The plugins' contents (and the text encoding) are part
of the cell cache key and the texture cache key: another plugin set converts again.

## 2. Records

- A record with the same type and ID as an earlier one replaces it whole. TES3 has no field-level merge.
- A record holding a `DELE` subrecord removes the earlier one.
- Keys: the ID (`NAME`) for most types; `INDX` for skills and magic effects; the `SCHD` name for scripts; path
  grids by their cell. Leveled lists are records like any other: a plugin's list replaces the whole list.
- Topics (`DIAL`) keep the first file's letter case (OpenMW prints it); a later file sets their type. A deleted topic
  goes with its responses.

## 3. Cells

- Interior cells are one cell by name, exterior ones by grid. A later file's `CELL` adds to the earlier one, it
  does not replace it. Its header fields (name, `DATA`, water, `AMBI`, region, map colour) take the later file's
  values where it has them; the ones it leaves out stay.
- A reference is known by its number (`FRMR`) and the file it comes from. The top byte of the number, when it is
  1..(number of masters), picks one of the file's own masters (1 = its first `MAST`), and the reference is that
  master's with the low 24 bits as its number; otherwise it is the file's own (OpenMW's `adjustRefNum`).
- A reference with the same key as an earlier one replaces it in place (the cell's order is kept); one with `DELE`
  is deleted; one with a new key is added after the cell's earlier references.
- `MVRF` (a reference's number, keyed the same way) followed by `CNDT` (an exterior grid) moves a reference to that
  cell: the reference that follows the pair is the moved one's data. It is no longer listed in its first cell, and
  it is listed in the target cell after that cell's own (OpenMW's leased references). A later move of the same
  reference wins. An exterior cell that only a moved reference names is made, empty, with water.
- Saves name a reference by its cell and index there, checked against its ID: a plugin added or removed later
  shifts the indices of cells it touches, and the save then drops those references' state rather than giving it to
  another object.

## 4. Dialogue responses

`INFO`s of a topic are ordered by their `PNAM` (the one before) as OpenMW's `InfoOrder` does:
- An `INFO` whose `INAM` is already in the topic with the same `PNAM` replaces it where it stands.
- Otherwise it goes right after the `INFO` its `PNAM` names; first when `PNAM` is empty; last when `PNAM` names
  none known. An `INFO` already there with another `PNAM` moves to that place.
- A deleted `INFO` keeps its place until every file is read (a later `INFO` may name it as `PNAM`), then goes.
- Order decides which response fires (the first that passes its filters).

## 5. Land

- `LAND` with the same grid: the later file's whole record.
- `LTEX`: a `LAND`'s `VTEX` values (index + 1, 0 = the default texture) name the land textures of the *same file*
  (OpenMW maps plugin and index to an `LTEX` ID). A later `LTEX` with the same ID gives every land using it its
  texture. We keep `Morrowind.esm`'s indices, give a plugin's new `LTEX` IDs the next ones, and rewrite a plugin's
  `VTEX` into that one index space; a value naming no `LTEX` of its file is the default texture.

## 6. Text

- Record text is 8-bit in the release's code page. `MW3DS_ENCODING` takes OpenMW's `--encoding` names: `win1252`
  (default: English, German, French, Italian, Spanish), `win1250` (Polish, Czech, Hungarian), `win1251` (Russian).
  The converter decodes every game string with it (IDs, names, dialogue, books, scripts, `Morrowind.ini`, the
  font's glyph slots) and writes UTF-8; the engine draws UTF-8. File paths inside NIFs and BSAs stay as before.
- English data gains too: bytes 0x80-0x9F are curly quotes and dashes in win1252 (one ID changes: the spell
  `wulfharth's cups` now has a real apostrophe, as OpenMW shows it).
- Converted file names keep `[a-z0-9_]`; a name with letters outside ASCII gets `_` and a CRC of the name, or
  Polish and Cyrillic cell names would collide. ASCII names are unchanged.
- IDs compare without letter case for ASCII only, as OpenMW's `lowerCase` does: "Ä" and "ä" are different letters
  in a topic search there too.

## 7. Interface text

Morrowind's menus take their words from `s*` GMSTs, which a translation carries. Where our 3DS screens show a word a
GMST holds with the same English text (Close, Magic, Journal, Map, Stats, Bounty, Soul, Use, Equip, the schools,
the cast types, ranges, months, Major / Minor / Misc Skills ...), it comes from the GMST, the English word as
fallback. A GMST's spaces at its edges are dropped (`sMap` is "Map "). Words with no GMST of the same English text
stay English (Save and quit, Credits, the 3DS controls help): differs (3DS UI, kept).

## 8. A translation's own files

Next to its `Morrowind.esm` a release may have `Morrowind.cel`, `.top` and `.mrk`: lines `key<TAB>value` in its
code page (OpenMW's `Translation::Storage`; first line for a key wins).
- `.cel`: cell ID -> name shown. IDs stay for scripts, doors and file names; the name is shown in the HUD, the
  menu's header, map titles and door markers, door tooltips and the travel list (OpenMW's `#{sCell=...}` places).
- `.top`: phrase -> topic. An explicit link `@phrase#` in a response links the topic the phrase stands for.
- `.mrk`: topic -> the word that names it in text (searched instead of the topic's name).

## 9. Hypertext

A response or journal text is shown as OpenMW's `parseHyperText` builds it: `@name#` is an explicit link, shown as
`name` without the marks and without trailing pseudo asterisks (byte 0x7F; each stands for a `*` in the topic's
name); its topic is `.top`'s for the name, else the name. The text between links is searched for topic words as
before (`dialogue.md`). Topics named either way become known (`addTopicsFromText`). Polish releases mark topics
this way (yampt's "Apply Topic Tags").
