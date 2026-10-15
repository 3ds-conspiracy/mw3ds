# Playing in another language

MW3DS converts the game from your own Morrowind install. If your copy is a German, French, Polish, Russian or
other translated release, the converter can keep its language: names, dialogue, books, the journal and most menu
words come from your game files, not from MW3DS.

## What you need

- Your translated Morrowind: its `Morrowind.esm` (and `Morrowind.bsa`) in `Data Files`, as the release installed them.
- Its fonts, as the release installed them (in `Morrowind.bsa` or `Data Files\Fonts`). MW3DS draws text with your
  game's own font, so the letters of your language come from it.
- Only `Morrowind.esm` is used: Tribunal and Bloodmoon are not part of MW3DS.

## Steps

1. Find your language's code page in this table:

   | `MW3DS_ENCODING` | Languages |
   |---|---|
   | `win1252` (the default) | English, German, French, Italian, Spanish |
   | `win1250` | Polish, Czech, Hungarian |
   | `win1251` | Russian |

   These are the names OpenMW uses for its `--encoding` setting. If you play your copy in OpenMW, use the same one.

2. Put it in `.env`, for example for Russian:

   ```
   MW3DS_ENCODING=win1251
   ```

3. Convert and build as usual (see [README.md](README.md)). Converting again after changing the setting is required:
   the converted data holds the text.

## What changes language

| Comes from your game files | Stays English |
|---|---|
| Item, NPC, creature, spell and place names | Labels that only exist on the 3DS (Save and quit, Credits, the controls help, Effects volume ...) |
| Dialogue, books, notes, the journal | Messages for missing data ("... is not converted yet") |
| Menu words Morrowind has its own strings for: Close, Back, Magic, Map, Stats, Journal, Bounty, Soul, Use, Equip, schools, ranges, months, skill groups ... | The test and log text (only developers see these) |

Some releases ship extra files next to `Morrowind.esm`. MW3DS reads them when they are there, as OpenMW does:

| File | What it does |
|---|---|
| `Morrowind.cel` | Cell names as shown (the Russian release keeps the English cell names inside the game and shows these) |
| `Morrowind.mrk` | The word in the text that links to a dialogue topic |
| `Morrowind.top` | Other forms of a topic's name that link to it |

Topic links written as `@topic#` in the text (Polish releases use them) are shown as links without the marks.

## Plugins and yampt

Mods (`.esp`) work in translated games too: list them in `MW3DS_PLUGINS` (see the README). An English mod that
names English cells or topics may not match a translated `Morrowind.esm`; translators use
[yampt](https://github.com/raffll/yampt) to convert such mods, and a yampt patch plugin works like any other plugin.

## If something looks wrong

- **Letters are boxes, question marks or the wrong letters:** `MW3DS_ENCODING` does not match your release.
  Set the right one and convert again.
- **The game starts in the wrong place or character creation is missing:** your release may rename the start
  cells in a way the converter did not expect. Please open an issue and say which release you have.
- **Text runs past the edge of a button:** some languages have longer words than the 3DS buttons were sized for.
  Please open an issue with a screenshot.

Translated releases have not been tested on hardware yet. Reports from players with a translated copy are very
welcome.
