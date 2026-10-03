# Findings files

One file per audit area, written by a spec agent from OpenMW's source (never from our engine code), then read by
the fix agent for that area. Fixed rows move to "Mismatches" with the commit.

Format:
- `## Rules`: one heading per rule: OpenMW file:function, the rule in our own words, GMSTs, quirks, the stance on
  vanilla bugs.
- `## Tests written`: files, what each checks.
- `## Hooks needed in the engine`: EXPECT kinds / tokens that don't exist yet (name, args, what it returns).
- `## Open questions`.
- `## Mismatches`: filled in by the fix phase (rule, our behaviour, cause, fixed / differs on purpose, commit).
