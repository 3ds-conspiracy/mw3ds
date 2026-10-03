"""Spec-driven dialogue tests (spec/dialogue.md; OpenMW mwdialogue/filter.cpp, selectwrapper.cpp,
dialoguemanagerimp.cpp, keywordsearch.cpp, components/interpreter/defines.cpp).

An oracle of OpenMW's response filter over the converted dialogue data (out/world/game_dialogue_*.json,
game_actors_*.json, game_factions.json, game_journal_*.json): for a sampled (NPC, topic, player state) it works
out which response OpenMW would choose, then writes tools/tests/openmw-spec-dialogue.txt, which sets that player
state in the engine and EXPECTs the answer.

  python tools/specgen_dialogue.py [--seed N] [--count N]    writes tools/tests/openmw-spec-dialogue.txt
  python tools/specgen_dialogue.py --selftest                 checks the oracle's own pieces (no files written)

Three-valued logic: a condition about something the harness can't set or read (the player's race and sex, worn
clothing, item counts, locals, Detected ...) is "unknown"; a case whose answer depends on one is not written.
The engine's own code is never read: only what the data says and what the spec page says.
"""
import argparse
import json
import random
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import specdata  # noqa: E402
from openmw_specgen import Test, ATTRS, SKILLS, load_actors  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / "tools" / "tests"
UNKNOWN = object()      # the third truth value (None is "no response")
PC_IDENTITY = (None, None)      # (race lowercase, female 0/1) of the player when the run says so (--pc-race / --pc-female)

# ESM function numbers (the data's two-digit codes) -> what they read
ATTR_FUNC = {10: "strength", 51: "intelligence", 52: "willpower", 53: "agility", 54: "speed", 55: "endurance",
             56: "personality", 57: "luck"}
SKILL_FUNC = {11 + i: s for i, s in enumerate(SKILLS)}
# a fresh game's chargen leaves these quests started; their baseline index is not 0
BASELINE_UNKNOWN_QUESTS = {"a1_1_findspymaster"}
# globals the engine itself rewrites (updateDialogueGlobals, the clock, the random one): never set, never trusted
ENGINE_GLOBALS = {"pchascrimegold", "pchasgolddiscount", "crimegolddiscount", "crimegoldturnin", "pchasturnin",
                  "random100", "gamehour", "day", "month", "year", "dayspassed", "timescale"}
SEPARATORS = "\n\r \t'\"(["          # keywordsearch.cpp highlightKeywords: what may precede a keyword


# ---------------------------------------------------------------- the data

class World:
    def __init__(self):
        self.topics = {}                # lowercase name -> {"name", "type", "infos": [...]}
        for i in range(40):
            try:
                for t in specdata.load(f"game_dialogue_{i}.json")["dialogue"]:
                    k = t["name"].lower()
                    if k in self.topics:
                        self.topics[k]["infos"].extend(t["infos"])
                    else:
                        self.topics[k] = dict(t)
            except FileNotFoundError:
                break
        self.actors = load_actors()
        self.factions = specdata.load("game_factions.json")["factions"]
        g = specdata.load("game.json")
        self.cells = g["cells"]
        self.globals = {k.lower(): v for k, v in g["globals"].items()}
        self.ref_cells = specdata.load("game_ref_cells.json")["ref_cells"]
        self.races = {r["id"].lower(): r["name"] for r in specdata.load("game_races.json")["races"]}
        self.classes = {c["id"].lower(): c["name"] for c in specdata.load("game_classes.json")["classes"]}
        self.journal = {}
        for i in range(40):
            try:
                self.journal.update(specdata.load(f"game_journal_{i}.json")["journal"])
            except FileNotFoundError:
                break
        # speakers' locals: a declared local that no script and no dialogue result script mentions keeps its initial 0
        self.script_locals, text, self.script_body = {}, [], {}
        for i in range(40):
            try:
                for k, v in specdata.load(f"game_scripts_{i}.json")["scripts"].items():
                    self.script_locals[k.lower()] = {n.lower() for _, n in v["locals"]}
                    text.append(json.dumps(v["body"]).lower())
                    self.script_body[k.lower()] = text[-1]
            except FileNotFoundError:
                break
        dtext = []
        for t in self.topics.values():
            for inf in t["infos"]:
                dtext.append(json.dumps(inf["script"]).lower())
        self.script_text = " ".join(text)
        self.dialogue_text = " ".join(dtext)
        # topics with an info whose Not-ID/-Faction/-Class/-Race/-Cell condition is not "= 1": the converter's
        # compare() may have dropped infos OpenMW would keep (it ignores operator and value), so they are skipped
        self.tainted = set()
        for k, t in self.topics.items():
            for inf in t["infos"]:
                if any(c[0] in "789AB" and not (c[2] == "0" and c[3] == 1) for c in inf["conds"]):
                    self.tainted.add(k)

    def journal_indexes(self, quest):
        q = self.journal.get(quest)
        return sorted({e["index"] for e in q["entries"]}) if q else []

    def npc_cell(self, actor):
        """(cell name, file-ish GOTO token) for an NPC placed in exactly one interior cell, else None"""
        cs = self.ref_cells.get(actor["id"].lower())
        if not cs or len(cs) != 1:
            return None
        c = self.cells[cs[0]]
        if not c.get("interior"):
            return None
        return c["name"]


# ---------------------------------------------------------------- the player's state (what the harness sets)

class PC:
    def __init__(self):
        self.journal = {}       # quest -> index
        self.globals = {}       # name -> value
        self.attrs = {}
        self.skills = {}
        self.level = None
        self.rep = None
        self.bounty = None
        self.factions = {}      # faction id (lowercase) -> rank
        self.disp = None        # the speaker's disposition towards the player (SETDISP)
        self.choice = -1


def cmp(op, a, b):
    return {"0": a == b, "1": a != b, "2": a > b, "3": a >= b, "4": a < b, "5": a <= b}[op]


def tri_and(values):
    """False wins, then unknown, else True"""
    if any(v is False for v in values):
        return False
    if any(v is UNKNOWN for v in values):
        return UNKNOWN
    return True


# ---------------------------------------------------------------- the filter (filter.cpp)

class Ctx:
    def __init__(self, w, actor, cell, pc, talked=False):
        self.w, self.a, self.cell, self.pc, self.talked = w, actor, cell, pc, talked
        self.fac = actor["faction"].lower()
        self.world = {}         # a guess for the boolean unknowns: pcfemale, samerace, detected, shouldattack

    # testActor (filter.cpp): the speaker-only fields of an info
    def who_ok(self, who):
        a = self.a
        if "id" in who and who["id"] != a["id"].lower():
            return False
        if "race" in who and who["race"] != a["race"].lower():
            return False
        if "class" in who and who["class"] != a["class"].lower():
            return False
        fac = who.get("faction")
        if fac == "ffff":
            if self.fac:
                return False
        elif fac:
            if self.fac != fac:
                return False
            if "rank" in who and a["rank"] < who["rank"]:
                return False
        elif "rank" in who:
            if (a["rank"] if self.fac else -1) < who["rank"]:
                return False
        if "sex" in who and who["sex"] == (0 if a["female"] else 1):
            return False
        # testPlayer: the PLAYER's cell, by name prefix
        if "cell" in who and not self.cell.lower().startswith(who["cell"]):
            return False
        return True

    def pc_rank(self, fac):
        return self.pc.factions.get(fac, -1) if fac else -1

    def func(self, code):
        """value of a Select function, or UNKNOWN"""
        a, pc, c = self.a, self.pc, int(code)
        if c in (0, 1):                                     # faction reaction lowest / highest over the PC's factions
            if not self.fac:
                return 0
            react = self.w.factions.get(self.fac, {}).get("reactions", {})
            vals = [react.get(f, 0) for f in pc.factions]
            return min([0] + vals) if c == 0 else max([0] + vals)
        if c == 2:                                          # Rank Requirement
            return 0 if not self.fac else UNKNOWN
        if c == 3:
            return a["reputation"]
        if c == 4:
            return 100                                      # a fresh speaker is unhurt
        if c == 5:
            return pc.rep if pc.rep is not None else UNKNOWN
        if c == 6:
            return pc.level if pc.level is not None else UNKNOWN
        if c == 7:
            return 100                                      # (assumed: a loaded game starts with full health)
        if c in (8, 9, 64):
            return UNKNOWN                                  # PC magicka / fatigue / health: absolute, class-dependent
        if c in ATTR_FUNC:
            return pc.attrs.get(ATTR_FUNC[c], UNKNOWN)
        if c in SKILL_FUNC:
            return pc.skills.get(SKILL_FUNC[c], UNKNOWN)
        if c == 38:
            return self.world.get("pcfemale", UNKNOWN)      # PC gender
        if c in (39, 40, 41, 58, 60, 73):
            return 0                                        # expelled, common / blight disease, corprus, vampire, werewolf kills
        if c == 42:
            return UNKNOWN                                  # clothing modifier: value of worn slots 0-15
        if c == 43:
            return pc.bounty if pc.bounty is not None else UNKNOWN
        if c == 44:                                         # same sex
            return UNKNOWN if "pcfemale" not in self.world else int(self.world["pcfemale"] == a["female"])
        if c == 45:
            return self.world.get("samerace", UNKNOWN)      # same race
        if c == 46:
            return 1 if (self.fac and self.fac in pc.factions) else 0
        if c == 47:
            if not self.fac:
                return 0
            return self.pc_rank(self.fac) - a["rank"]
        if c == 48:
            return self.world.get("detected", UNKNOWN)      # Detected: awareness check
        if c == 49 or c == 62 or c == 65 or c == 66 or c == 72:
            return 0                                        # alarmed, attacked, creature target, friend hit, werewolf
        if c == 50:
            return pc.choice
        if c == 59:
            return UNKNOWN                                  # (handled by the caller: false indoors)
        if c == 61:
            return a["level"]
        if c == 63:
            return 1 if self.talked else 0
        if c == 67:
            return a["fight"]
        if c == 68:
            return a["hello"]
        if c == 69:
            return a["alarm"]
        if c == 70:
            return a["flee"]
        if c == 71:
            return self.world.get("shouldattack", UNKNOWN)  # Should Attack: isAggressive
        return UNKNOWN

    def cond(self, k):
        kind, key, op, val = k
        a, pc, w = self.a, self.pc, self.w
        if kind == "1":
            if int(key) == 50 and pc.choice == -1:
                return False                                # no choice in progress: every Choice condition fails
            if int(key) == 59:
                return False                                # interiors: weather conditions fail
            v = self.func(key)
            return UNKNOWN if v is UNKNOWN else cmp(op, v, val)
        if kind == "2":
            if key not in w.globals:
                return True                                 # a global that doesn't exist: the condition is ignored
            if key in ENGINE_GLOBALS:
                return UNKNOWN
            if key in pc.globals:
                return cmp(op, pc.globals[key], val)
            return UNKNOWN
        if kind in "3C":                                    # local variable of the speaker's script
            r = self.local(key, op, val)
            return r if (r is UNKNOWN or kind == "3") else not r
        if kind == "4":
            idx = pc.journal.get(key)
            if idx is None:
                if key in BASELINE_UNKNOWN_QUESTS:
                    return UNKNOWN
                idx = 0
            return cmp(op, idx, val)
        if kind == "5":
            return UNKNOWN                                  # item count: the harness can't set an exact count yet
        if kind == "6":
            return cmp(op, 0, val)                          # nobody has died yet
        if kind == "7":
            return a["id"].lower() != key                   # operator and value are ignored
        if kind == "8":
            return self.fac != key
        if kind == "9":
            return a["class"].lower() != key
        if kind == "A":
            return a["race"].lower() != key
        if kind == "B":
            return not self.cell.lower().startswith(key)    # (the SPEAKER's cell: the same cell here)
        if kind == "F":                                     # DNAM + PC rank: the player belongs, at that rank or above
            return key in pc.factions and pc.factions[key] >= val
        if kind == "G":                                     # PC rank without a faction: the speaker's faction
            return bool(self.fac) and self.fac in pc.factions and pc.factions[self.fac] >= val
        return UNKNOWN

    def local(self, name, op, val):
        """(Not)Local: False without a script or when the script has no such variable; 0 when it is declared and
        nothing anywhere writes or reads it by name; else unknown"""
        sc = self.a["script"].lower()
        if not sc or name not in self.w.script_locals.get(sc, set()):
            return False
        if (f'"{name}"' in self.w.script_body.get(sc, "") or f'"{name}"' in self.w.dialogue_text
                or f'"r", "{self.a["id"].lower()}", "{name}"' in self.w.script_text):
            return UNKNOWN
        return cmp(op, 0, val)

    def info(self, inf):
        return tri_and([self.who_ok(inf["who"])] + [self.cond(k) for k in inf["conds"]])

    def disp_ok(self, inf, invert=False):
        d = self.pc.disp
        if d is None:
            return UNKNOWN
        return (inf["disp"] == 0 or d < inf["disp"]) if invert else d >= inf["disp"]

    def search(self, topic, fallback=True, refusal=None):
        """the answer in every guess of the boolean unknowns; only an answer they all agree on is returned"""
        return self._agree(lambda: self._search(topic, fallback, refusal))

    def _agree(self, fn):
        import itertools
        res = []
        pcr, pcf = PC_IDENTITY
        for f, r, d, sh in itertools.product((0, 1), repeat=4):
            if pcf is not None and f != pcf:
                continue
            if pcr is not None and r != int(pcr == self.a["race"].lower()):
                continue
            self.world = {"pcfemale": f, "samerace": r, "detected": d, "shouldattack": sh}
            x = fn()
            if x is UNKNOWN:
                self.world = {}
                return UNKNOWN
            res.append(x)
        self.world = {}
        first = res[0]
        for x in res[1:]:
            if (x is None) != (first is None) or (x and x[0] is not first[0]):
                return UNKNOWN
        return first

    def _search(self, topic, fallback=True, refusal=None):
        """OpenMW Filter::search: (info, topic dict) of the first response, None for none, UNKNOWN when the answer
        hangs on something unknown"""
        refused = False
        for inf in topic["infos"]:
            s = self.info(inf)
            if s is False:
                continue
            if s is UNKNOWN:
                return UNKNOWN
            ok = self.disp_ok(inf)
            if ok is UNKNOWN:
                return UNKNOWN
            if ok:
                return inf, topic
            refused = True
        if fallback and refused and refusal is not None:
            for inf in refusal["infos"]:
                s = self.info(inf)
                if s is False:
                    continue
                ok = self.disp_ok(inf)
                if s is UNKNOWN or ok is UNKNOWN:
                    return UNKNOWN
                if ok:
                    return inf, refusal
        return None


def greeting(ctx, w):
    """startDialogue: greeting topics in sorted id order, the first with a response (no Info Refusal)"""
    names = sorted(k for k, t in w.topics.items() if t["type"] == 2)

    def run():
        for k in names:
            if k in w.tainted:
                return UNKNOWN              # (the converter may have dropped infos of this topic)
            r = ctx._search(w.topics[k], fallback=False)
            if r is UNKNOWN:
                return UNKNOWN
            if r:
                return r
        return None
    return ctx._agree(run)


# ---------------------------------------------------------------- % substitution (defines.cpp fixDefinesDialog)

DIALOGUE_ORDER = ["nextpcrank", "pcnextrank", "faction", "pcclass", "pcname", "pcrace", "pcrank", "class", "cell",
                  "race", "rank", "name"]
HOLE = "\x00"       # a substitution the oracle can't evaluate


def expand(text, ctx, global_names=()):
    """ctx: {builtin keyword: text or None (unknown)}; "pccrimelevel" likewise. The quirk kept: after an
    unmatched escape character the next character is never looked at as an escape."""
    out, start, i = [], 0, 0
    gl = sorted(global_names, key=len, reverse=True)
    while i < len(text):
        ch = text[i]
        if ch in "%^":
            out.append(text[start:i])
            rest = text[i + 1:].lower()
            hit = None
            if rest.startswith("pccrimelevel"):
                hit = ("pccrimelevel", len("pccrimelevel"))
            else:
                for n in DIALOGUE_ORDER:
                    if rest.startswith(n):
                        hit = (n, len(n))
                        break
            if hit is None:
                for g in gl:
                    if rest.startswith(g):
                        hit = ("global:" + g, len(g))
                        break
            if hit is not None:
                v = ctx.get(hit[0])
                out.append(HOLE if v is None else str(v))
                i += hit[1]
                start = i + 1
            else:
                out.append(ch)
                i += 1
                start = i
        i += 1
    out.append(text[start:])
    return "".join(out)


def subst_ctx(w, actor, cell, pc):
    fac = actor["faction"].lower()
    f = w.factions.get(fac)
    ranks = f["ranks"] if f else []
    c = {"name": actor["name"], "race": w.races.get(actor["race"].lower()), "class": w.classes.get(actor["class"].lower()),
         "cell": cell, "faction": f["name"] if f else None,
         "rank": ranks[actor["rank"]] if f and 0 <= actor["rank"] < len(ranks) else None,
         "pccrimelevel": pc.bounty if pc.bounty is not None else None}
    return c


# ---------------------------------------------------------------- keywords in a response (keywordsearch.cpp)

def find_keywords(text, names):
    """names: lowercase topic names. Returns the topic names found, left to right: a keyword is looked for where the
    text starts or after one of SEPARATORS (its end is NOT checked), the longest non-overlapping first, plus
    @explicit# links."""
    out, pos = [], 0
    runs = []
    while True:
        b = text.find("@", pos)
        e = text.find("#", b) if b != -1 else -1
        if b != -1 and e != -1:
            if b != pos:
                runs.append((text[pos:b], pos))
            out.append((b, text[b + 1:e].lower()))
            pos = e + 1
        else:
            if pos < len(text):
                runs.append((text[pos:], pos))
            break
    for seg, off in runs:
        ms = []
        low = seg.lower()
        for i in range(len(seg)):
            if i and seg[i - 1] not in SEPARATORS:
                continue
            for n in names:
                if low.startswith(n, i):
                    ms.append((i, i + len(n), n))
        while ms:
            best = ms[0]
            for m in ms:
                if m[1] - m[0] > best[1] - best[0]:
                    best = m
            ms.remove(best)
            out.append((off + best[0], best[2]))
            ms = [m for m in ms if not (m[0] < best[1] and m[1] > best[0])]
    out.sort()
    return [n for _, n in out]


# ---------------------------------------------------------------- snippets for EXPECT lines

OKCH = re.compile(r"[a-z0-9 ,.'!?-]")


def runs_of(text):
    """clean runs of a lowercased text: ASCII a-z 0-9 space , . ' ! ? - only"""
    cur, runs = [], []
    for ch in text.lower():
        if OKCH.match(ch):
            cur.append(ch)
        else:
            if cur:
                runs.append("".join(cur))
            cur = []
    if cur:
        runs.append("".join(cur))
    return runs


def snippet(text, others, around=None, width=34, minlen=14):
    """a lowercase piece of text, clean for a token, found in none of the other texts; None if there isn't one"""
    low_others = [o.lower() for o in others]
    cand = []
    for r in runs_of(text):
        r = r.strip()
        if len(r) < minlen:
            continue
        for s in range(0, max(1, len(r) - width + 1), 6):
            p = r[s:s + width].strip()
            if len(p) >= minlen and p == r[s:s + width].strip():
                cand.append(p)
    if around:
        cand.sort(key=lambda p: 0 if around in p else 1)
    for p in cand:
        if not any(p in o for o in low_others):
            return p
    return None


def tok(s):
    return s.replace(" ", "_")


# ---------------------------------------------------------------- sampling a player state

def pick_near(rng, v, lo, hi):
    return max(lo, min(hi, v + rng.choice([-1, 0, 0, 1, 2])))


def sample_state(w, rng, actor, cands, pc_disp_choices):
    """a PC with every value the candidates' conditions read set, near their thresholds"""
    pc = PC()
    fac = actor["faction"].lower()
    facs = set()
    for inf in cands:
        for kind, key, op, val in inf["conds"]:
            if kind == "4":
                idxs = w.journal_indexes(key)
                if idxs:
                    pool = [i for i in idxs if abs(i - val) <= 10] or idxs
                    n = rng.choice(pool)
                    if rng.random() < 0.25:
                        n = 0
                    if n:
                        pc.journal[key] = n
                    else:
                        pc.journal.pop(key, None)
            elif kind == "2" and key in w.globals and key not in ENGINE_GLOBALS:
                pc.globals[key] = pick_near(rng, int(val), -1, 10 ** 6)
            elif kind == "1":
                c = int(key)
                if c in ATTR_FUNC:
                    pc.attrs[ATTR_FUNC[c]] = pick_near(rng, int(val), 5, 100)
                elif c in SKILL_FUNC:
                    pc.skills[SKILL_FUNC[c]] = pick_near(rng, int(val), 5, 100)
                elif c == 5:
                    pc.rep = pick_near(rng, int(val), 0, 100)
                elif c == 6:
                    pc.level = pick_near(rng, int(val), 1, 60)
                elif c == 43:
                    pc.bounty = max(0, int(val) + rng.choice([-1, 0, 1, 50]))
                elif c in (0, 1, 46, 47) and fac:
                    facs.add(fac)
            elif kind == "F":
                facs.add(key)
            elif kind == "G" and fac:
                facs.add(fac)
    for f in sorted(facs):
        fd = w.factions.get(f)
        if fd and rng.random() < 0.6:
            pc.factions[f] = rng.randint(0, max(0, len(fd["ranks"]) - 1))
    if any(int(k[1]) == 43 for inf in cands for k in inf["conds"] if k[0] == "1") and pc.bounty is None:
        pc.bounty = 0
    ds = sorted({inf["disp"] for inf in cands} | {0, 50, 100})
    base = rng.choice(ds)
    pc.disp = max(0, min(100, base + rng.choice([-1, 0, 0, 1])))
    return pc


def state_tokens(pc, w):
    toks = []
    for f, r in sorted(pc.factions.items()):
        toks.append(f"JOIN:{tok(f)}:{r}")
    for q, n in sorted(pc.journal.items()):
        toks.append(f"JOURNAL:{q}:{n}")
    for g, v in sorted(pc.globals.items()):
        toks.append(f"SETGLOBAL:{g}:{v}")
    for n, v in sorted(pc.attrs.items()):
        toks.append(f"SETATTR:{n}:{v}")
    for n, v in sorted(pc.skills.items()):
        toks.append(f"SETSKILL:{n}:{v}")
    if pc.level is not None:
        toks.append(f"LEVEL:{pc.level}")
    if pc.rep is not None:
        toks.append(f"SETREP:{pc.rep}")
    if pc.bounty is not None:
        toks.append(f"SETBOUNTY:{pc.bounty}")
    return toks


# ---------------------------------------------------------------- the generator

def tokname(s):
    return s.replace(" ", "_")


def usable_actors(w):
    out = []
    for a in w.actors.values():
        if not a["race"] or not a["class"] or ":" in a["id"] or "+" in a["id"] or "%" in a["id"]:
            continue
        cell = w.npc_cell(a)
        if cell and not any(ch in cell for ch in ":+%"):
            out.append((a, cell))
    return out


def cell_token(cell):
    return cell.replace(" ", "_")


def new_case(t, cell, slot):
    t.step(f"LOAD:{slot}", 0.5)
    t.wait(3)
    if PC_IDENTITY[0] is not None:
        t.step(f"PCRACE:{tok(PC_IDENTITY[0])}", 0.2)         # (hook: not in the engine yet)
    if PC_IDENTITY[1] is not None:
        t.step(f"PCSEX:{PC_IDENTITY[1]}", 0.2)               # (hook: not in the engine yet)
    t.step(f"GOTO:{cell_token(cell)}", 0.3)
    t.wait(8)


def gen(rng, count):
    w = World()
    actors = usable_actors(w)
    topic_names = sorted(k for k, t in w.topics.items() if t["type"] == 0 and k not in w.tainted)
    gl = sorted(w.globals)
    t = Test()
    t.step("CLASS:battlemage")
    t.step("BOOST:100")
    t.step("SAVE:specdialogue", 1)
    stats = {"topic": 0, "greeting": 0, "learn": 0, "skipped": 0, "none": 0, "refusal": 0}
    refusal = w.topics.get("info refusal")
    greet_ok = True
    all_names = [k for k in topic_names]
    guard = 0
    seen = set()
    while sum(stats[k] for k in ("topic", "greeting", "learn")) < count and guard < count * 3000:
        guard += 1
        actor, cell = rng.choice(actors)
        kind = rng.choices(["topic", "greeting", "learn"], [6, 3, 4])[0]
        ctx0 = Ctx(w, actor, cell, PC())
        if kind == "greeting":
            if not greet_ok:
                continue
            tps = [tp for tp in w.topics.values() if tp["type"] == 2]
            cands = [inf for tp in tps for inf in tp["infos"] if ctx0.who_ok(inf["who"])]
        else:
            tn = rng.choice(topic_names)
            tp = w.topics[tn]
            cands = [inf for inf in tp["infos"] if ctx0.who_ok(inf["who"])]
        if len(cands) < 2:
            continue
        pc = sample_state(w, rng, actor, cands, None)
        ctx = Ctx(w, actor, cell, pc)
        if kind == "greeting":
            r = greeting(ctx, w)
            texts_all = [inf["text"] for tp2 in tps for inf in tp2["infos"]]
            sc = subst_ctx(w, actor, cell, pc)
            if r is UNKNOWN or r is None:
                continue
            inf, tp_hit = r
            exp = expand(inf["text"], sc, gl)
            others = [expand(x, sc, gl) for x in texts_all if x is not inf["text"]]
            sn = snippet(exp, others, minlen=8)
            if not sn:
                continue
            if ("g", actor["id"], sn) in seen:
                continue
            seen.add(("g", actor["id"], sn))
            new_case(t, cell, "specdialogue")
            for s in state_tokens(pc, w):
                t.step(s, 0.2)
            t.step(f"SETDISP:{tokname(actor['id'])}:{pc.disp}", 0.2)
            t.step(f"EXPECT:greeting:{tokname(actor['id'])}:{tok(sn)}", 0.2)
            stats["greeting"] += 1
            continue
        r = ctx.search(tp, True, refusal)
        if r is UNKNOWN:
            continue
        nm = tokname(tp["name"])
        if (kind, actor["id"], nm, id(r[0]) if r else 0) in seen:
            continue
        seen.add((kind, actor["id"], nm, id(r[0]) if r else 0))
        if ":" in nm or "+" in nm or "%" in nm:
            continue
        pool = [i["text"] for i in tp["infos"]] + ([i["text"] for i in refusal["infos"]] if refusal else [])
        if r is None:
            # no response: none of the candidates' texts may be said
            ne = [snippet(inf["text"], [x for x in pool if x != inf["text"]]) for inf in cands[:2]]
            ne = [x for x in ne if x]
            if not ne:
                continue
            new_case(t, cell, "specdialogue")
            t.step(f"TALK:{tokname(actor['id'])}", 0.3)
            t.wait(1.5)
            for s in state_tokens(pc, w):
                t.step(s, 0.2)
            t.step(f"KNOW:{nm}", 0.2)
            t.step(f"SETDISP:{tokname(actor['id'])}:{pc.disp}", 0.2)
            for x in ne:
                t.step(f"EXPECT:answer:{nm}:{tokname(actor['id'])}:ne:{tok(x)}", 0.2)
            stats["none"] += 1
            stats["topic"] += 1
            continue
        inf, hit = r
        sn = snippet(inf["text"], [x for x in pool if x != inf["text"]])
        if not sn:
            continue
        # earlier candidates the oracle rejected: the engine must not say them
        before = []
        for c in tp["infos"]:
            if c is inf:
                break
            if ctx0.who_ok(c["who"]):
                x = snippet(c["text"], [inf["text"]])
                if x and x not in inf["text"].lower():
                    before.append(x)
        if kind == "learn":
            if inf["script"] or hit is not tp:
                continue
            found = [n for n in find_keywords(inf["text"], all_names) if n != tp["name"].lower()]
            learned = []
            for n in dict.fromkeys(found):
                r2 = ctx.search(w.topics[n], True, refusal)
                if r2 is UNKNOWN:
                    learned = None
                    break
                if r2:
                    learned.append(n)
            if not learned:
                continue
            new_case(t, cell, "specdialogue")
            t.step(f"TALK:{tokname(actor['id'])}", 0.3)
            t.wait(1.5)
            for s in state_tokens(pc, w):
                t.step(s, 0.2)
            t.step(f"KNOW:{nm}", 0.2)
            t.step(f"SETDISP:{tokname(actor['id'])}:{pc.disp}", 0.2)
            t.step(f"TOPIC:{nm}", 0.3)
            t.wait(1.5)
            t.step(f"EXPECT:said:{tok(sn)}", 0.2)
            for n in learned[:3]:
                if ":" not in n and "+" not in n:
                    t.step(f"EXPECT:topiclisted:{tokname(w.topics[n]['name'])}", 0.2)
            stats["learn"] += 1
            continue
        new_case(t, cell, "specdialogue")
        t.step(f"TALK:{tokname(actor['id'])}", 0.3)
        t.wait(1.5)
        for s in state_tokens(pc, w):
            t.step(s, 0.2)
        t.step(f"KNOW:{nm}", 0.2)
        t.step(f"SETDISP:{tokname(actor['id'])}:{pc.disp}", 0.2)
        t.step(f"EXPECT:answer:{nm}:{tokname(actor['id'])}:eq:{tok(sn)}", 0.2)
        for x in before[-2:]:
            t.step(f"EXPECT:answer:{nm}:{tokname(actor['id'])}:ne:{tok(x)}", 0.2)
        t.step(f"EXPECT:topiclisted:{nm}", 0.2)
        if hit is not tp:
            stats["refusal"] += 1
        stats["topic"] += 1
    n = t.save("openmw-spec-dialogue.txt")
    return n, stats


# ---------------------------------------------------------------- self-test

def selftest():
    sc = {"name": "Fargoth", "race": "Wood Elf", "class": "Thief", "faction": None, "rank": None, "cell": "Shop",
          "pccrimelevel": 250}
    assert expand("Hi %Name, a %race.", sc) == "Hi Fargoth, a Wood Elf."
    assert expand("%PCCrimeLevel gold", sc) == "250 gold"
    assert expand("%PCName", sc) == HOLE                    # unknown stays a hole
    assert expand("%Rank", sc) == HOLE
    assert expand("100% sure", sc) == "100% sure"            # no keyword: kept
    assert expand("%%name", sc) == "%%name"                  # the char after an unmatched % is never an escape
    assert expand("^cell!", sc) == "Shop!"
    assert expand("%Class%Name", sc) == "ThiefFargoth"
    assert expand("%classy", sc) == "Thiefy"                 # prefix match, nothing checks the end
    names = ["work", "the work", "bar"]
    assert find_keywords("Do the work, barkeep", names) == ["the work", "bar"]   # longest first; the end is not checked
    assert find_keywords("see @Work# now", names) == ["work"]
    assert find_keywords("homework", names) == []            # not after a separator
    # the filter
    w = type("W", (), {})()
    w.factions, w.globals = {"fac": {"reactions": {"other": -3}, "ranks": ["a", "b"], "name": "Fac"}}, {"g": {}}
    a = {"id": "x", "race": "Imperial", "class": "Guard", "faction": "Fac", "rank": 1, "female": 0, "reputation": 7,
         "level": 5, "script": "", "fight": 30, "hello": 10, "alarm": 0, "flee": 0}
    pc = PC()
    c = Ctx(w, a, "Balmora, Cell", pc)
    assert c.who_ok({"faction": "fac", "rank": 1}) and not c.who_ok({"faction": "fac", "rank": 2})
    assert not c.who_ok({"faction": "ffff"}) and not c.who_ok({"sex": 1}) and c.who_ok({"sex": 0})
    assert c.who_ok({"cell": "balmora"}) and not c.who_ok({"cell": "vivec"})
    assert c.cond(["1", "50", "0", 0]) is False             # Choice with no choice in progress
    assert c.cond(["1", "59", "3", 0]) is False             # Weather indoors
    assert c.cond(["2", "nosuch", "0", 5]) is True          # missing global ignored
    assert c.cond(["3", "v", "0", 0]) is False and c.cond(["C", "v", "0", 0]) is True
    assert c.cond(["7", "x", "1", 0]) is False and c.cond(["7", "y", "5", 9]) is True   # operator and value ignored
    assert c.func("47") == -2                               # PC not a member: rank -1, minus the speaker's 1
    pc.factions["fac"] = 3
    assert c.func("47") == 2
    pc.factions["other"] = 0
    assert c.func("01") == 0
    assert c.func("00") == -3
    top = {"infos": [{"who": {}, "conds": [], "disp": 60, "text": "a"}, {"who": {}, "conds": [], "disp": 0, "text": "b"}]}
    pc.disp = 50
    assert c.search(top)[0]["text"] == "b"                   # disposition too low for the first: the next one
    ref = {"infos": [{"who": {}, "conds": [], "disp": 0, "text": "no"}]}
    top2 = {"infos": [{"who": {}, "conds": [], "disp": 60, "text": "a"}]}
    assert c.search(top2, True, ref)[0]["text"] == "no" and c.search(top2, False, ref) is None
    print("selftest ok")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--count", type=int, default=40)
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--pc-race", help="the player's race (needs a PCRACE setup token in the engine: see the findings)")
    ap.add_argument("--pc-female", type=int, choices=[0, 1], help="the player's sex (PCSEX token)")
    args = ap.parse_args()
    if args.selftest:
        selftest()
    else:
        PC_IDENTITY = ((args.pc_race or "").lower() or None, args.pc_female)
        lines, st = gen(random.Random(args.seed), args.count)
        print(f"openmw-spec-dialogue.txt: {lines} lines; cases {st}")
