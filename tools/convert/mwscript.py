"""Morrowind script (MWScript) compiler: source text -> JSON-able syntax tree.

The 3DS interprets the tree directly. Node shapes:
  statements  ["set", target, expr]      target: ["l", name] | ["g", name] | ["r", ref, name]
              ["if", [[cond, body], ...], else_body or None]
              ["while", cond, body]
              ["ret"]
              ["c", ref or None, func, [args]]        function call (ref = explicit "x"->)
  expressions number | ["s", text] | ["l", name] | ["g", name] | ["r", ref, name]
              ["c", ref, func, [args]] | [op, a, b] (op in + - * / // == != < <= > >=) | ["neg", a]
              Whole numbers are written as integers, and "//" is a division of two integers (OpenMW's
              OpDivInt: 7 / 2 is 3); the engine truncates what is stored in short / long variables.
Names and function names are lowercase; local variables are resolved by name at run time.
"""
import re

TOKEN = re.compile(r'''
    (?P<str>"[^"\n]*"?)
  | (?P<id2>\d+[A-Za-z_'`][A-Za-z0-9_'`]*(?:-[A-Za-z0-9_'`]+)*)
  | (?P<num>\d+\.\d*|\.\d+|\d+)
  | (?P<op>->|==|!=|<>|<=|>=|=<|=>|<|>|=|\+|-|\*|/|\(|\)|\[|\]|,|\.)
  | (?P<id>[A-Za-z_][A-Za-z0-9_'`]*(?:-[A-Za-z0-9_'`]+)*)
''', re.VERBOSE)

# Functions usable inside expressions and how many arguments they take (default 0)
EXPR_ARITY = {
    "getdistance": 1, "getitemcount": 1, "getrace": 1, "getpccell": 1, "getdeadcount": 1,
    "getjournalindex": 1, "getspell": 1, "getspelleffects": 1, "geteffect": 1, "hasitemequipped": 1,
    "getsoundplaying": 1, "getlineofsight": 1, "getlos": 1, "getdetected": 1, "random": 1,
    "getangle": 1, "getpos": 1, "getstartingpos": 1, "getstartingangle": 1, "getsquareroot": 1,
    "getpcrank": 1, "getpcfacrep": 1, "getfactionreaction": 2, "getpcinjail": 0,
    "getbuttonpressed": 0, "getcurrentaipackage": 0, "getaipackagedone": 0, "getresistdisease": 0,
    "hitonme": 1, "hitattemptonme": 1, "gettarget": 1, "hassoulgem": 1, "pcexpelled": 1, "scriptrunning": 1,
}


FUNCS = {   # name: (result type, argument signature) from OpenMW's extensions: S c x string, l f number, X z optional extra, / the rest optional
    "cellchanged": ("l", ""), "getaipackagedone": ("l", ""), "getalarm": ("l", ""), "getangle": ("f", "c"),
    "getarmortype": ("l", "l"), "getattacked": ("l", ""), "getblightdisease": ("l", ""),
    "getbuttonpressed": ("l", ""), "getcollidingactor": ("l", ""), "getcollidingpc": ("l", ""),
    "getcommondisease": ("l", ""), "getcurrentaipackage": ("l", ""), "getcurrenttime": ("f", ""),
    "getcurrentweather": ("l", ""), "getdeadcount": ("l", "c"), "getdetected": ("l", "c"),
    "getdisabled": ("l", "x"), "getdisposition": ("l", ""), "getdistance": ("f", "c"),
    "geteffect": ("l", "S"), "getfactionreaction": ("l", "ccX"), "getfight": ("l", ""), "getflee": ("l", ""),
    "getforcejump": ("l", ""), "getforcemovejump": ("l", ""), "getforcerun": ("l", ""),
    "getforcesneak": ("l", ""), "gethello": ("l", ""), "getinterior": ("l", ""), "getitemcount": ("l", "cX"),
    "getjournalindex": ("l", "c"), "getlevel": ("l", ""), "getlineofsight": ("l", "c"),
    "getlocked": ("l", ""), "getlos": ("l", "c"), "getmasserphase": ("l", ""), "getpccell": ("l", "c"),
    "getpccrimelevel": ("f", ""), "getpcfacrep": ("l", "/c"), "getpcinjail": ("l", ""),
    "getpcjumping": ("l", ""), "getpcrank": ("l", "/S"), "getpcrunning": ("l", ""), "getpcsleep": ("l", ""),
    "getpcsneaking": ("l", ""), "getpctraveling": ("l", ""), "getpcvisionbonus": ("f", ""),
    "getpos": ("f", "c"), "getrace": ("l", "c"), "getreputation": ("l", ""), "getscale": ("f", ""),
    "getsecondspassed": ("f", ""), "getsecundaphase": ("l", ""), "getsoundplaying": ("l", "c"),
    "getspell": ("l", "c"), "getspelleffects": ("l", "c"), "getspellreadied": ("l", ""),
    "getsquareroot": ("f", "f"), "getstandingactor": ("l", ""), "getstandingpc": ("l", ""),
    "getstartingangle": ("f", "c"), "getstartingpos": ("f", "c"), "getstat": ("l", "c"),
    "gettarget": ("l", "c"), "getwaterlevel": ("f", ""), "getweapondrawn": ("l", ""),
    "getweapontype": ("l", ""), "getwerewolfkills": ("l", ""), "getwindspeed": ("f", ""),
    "hasitemequipped": ("l", "c"), "hassoulgem": ("l", "c"), "hitattemptonme": ("l", "S"),
    "hitonme": ("l", "S"), "iswerewolf": ("l", ""), "menumode": ("l", ""), "onactivate": ("l", ""),
    "ondeath": ("l", ""), "onknockout": ("l", ""), "onmurder": ("l", ""), "pcexpelled": ("l", "/S"),
    "pcget3rdperson": ("l", ""), "random": ("f", "l"), "repairedonme": ("l", "S"), "samefaction": ("l", ""),
    "saydone": ("l", ""), "scriptrunning": ("l", "c"), "xbox": ("l", ""),
}
INSTRS = {
    "activate": "x", "additem": "clX", "addsoulgem": "ccX", "addspell": "cz", "addtolevcreature": "ccl",
    "addtolevitem": "ccl", "addtopic": "S", "aiactivate": "c/l", "aiescort": "cffff/l",
    "aiescortcell": "ccffff/l", "aifollow": "cffff/llllllll", "aifollowcell": "ccffff/l",
    "aitravel": "fff/lx", "aiwander": "fff/llllllllll", "bc": "/S", "becomewerewolf": "", "betacomment": "/S",
    "cast": "SS", "centeroncell": "S", "centeronexterior": "ll", "changeweather": "Sl",
    "choice": "j/SlSlSlSlSlSlSlSlSlSlSlSlSlSlSlSlSlSlSlSlSlSlSlSl", "clearforcejump": "",
    "clearforcemovejump": "", "clearforcerun": "", "clearforcesneak": "", "clearinfoactor": "", "coc": "S",
    "coe": "ll", "disable": "x", "disablelevitation": "", "disableteleporting": "", "dontsaveobject": "",
    "drop": "cl", "dropsoulgem": "c", "enable": "x", "enablebirthmenu": "", "enableclassmenu": "",
    "enableinventorymenu": "", "enablelevelupmenu": "", "enablelevitation": "", "enablemagicmenu": "",
    "enablemapmenu": "", "enablenamemenu": "", "enableracemenu": "", "enablerest": "",
    "enablestatreviewmenu": "", "enablestatsmenu": "", "enableteleporting": "", "equip": "cX",
    "explodespell": "S", "face": "ffX", "fadein": "f", "fadeout": "f", "fadeto": "ff", "fall": "",
    "filljournal": "", "fillmap": "", "fixme": "", "forcegreeting": "z", "forcejump": "", "forcemovejump": "",
    "forcerun": "", "forcesneak": "", "goodbye": "", "gotojail": "", "help": "", "hurtcollidingactor": "f",
    "hurtstandingactor": "f", "journal": "cl", "lock": "/l", "loopgroup": "cl/l", "lowerrank": "x",
    "menutest": "/l", "modalarm": "l", "moddisposition": "l", "modfactionreaction": "ccl", "modfight": "l",
    "modflee": "l", "modhello": "l", "modpccrimelevel": "f", "modpcfacrep": "l/c", "modpcvisionbonus": "f",
    "modregion": "S/llllllllllX", "modreputation": "l", "modscale": "f", "modwaterlevel": "f", "move": "cf",
    "moveworld": "cf", "ori": "/S", "outputrefinfo": "/S", "payfine": "", "payfinethief": "",
    "pcclearexpelled": "/S", "pcexpell": "/S", "pcforce1stperson": "", "pcforce3rdperson": "",
    "pcjoinfaction": "/S", "pclowerrank": "/S", "pcraiserank": "/S", "placeatme": "clflX",
    "placeatpc": "clflX", "placeitem": "cffffX", "placeitemcell": "ccffffX", "playbink": "Sl",
    "playgroup": "c/l", "playloopsound3d": "cXX", "playloopsound3dvp": "cff", "playsound": "cXX",
    "playsound3d": "cXX", "playsound3dvp": "cff", "playsoundvp": "cff", "position": "ffffz",
    "positioncell": "ffffczz", "ra": "", "raiserank": "x", "reloadlua": "", "removeeffects": "l",
    "removefromlevcreature": "ccl", "removefromlevitem": "ccl", "removeitem": "clX", "removesoulgem": "c/l",
    "removespell": "cz", "removespelleffects": "c", "resetactors": "", "resurrect": "", "rotate": "cf",
    "rotateworld": "cf", "say": "SS", "setalarm": "l", "setangle": "cf", "setatstart": "", "setdelete": "l",
    "setdisposition": "l", "setfactionreaction": "ccl", "setfight": "l", "setflee": "l", "sethello": "l",
    "setjournalindex": "cl", "setlevel": "l", "setnavmeshnumber": "l", "setpccrimelevel": "f",
    "setpcfacrep": "l/c", "setpcvisionbonus": "f", "setpos": "cf", "setreputation": "l", "setscale": "f",
    "setwaterlevel": "f", "setwerewolfacrobatics": "", "show": "c", "showmap": "Sxxxx", "showrestmenu": "",
    "showscenegraph": "/l", "showvars": "", "skipanim": "", "ssg": "/l", "startcombat": "c",
    "startscript": "c", "stopcombat": "x", "stopscript": "c", "stopsound": "cXX", "streammusic": "S",
    "sv": "", "t3d": "", "tai": "", "tap": "", "tb": "", "tcb": "", "tcg": "", "tcl": "", "testcells": "",
    "testinteriorcells": "", "testmodels": "", "tfh": "", "tfow": "", "tgm": "", "tm": "",
    "toggleactorspaths": "", "toggleai": "", "toggleborders": "", "togglecollision": "",
    "togglecollisionboxes": "", "togglecollisiongrid": "", "togglefogofwar": "", "togglefullhelp": "",
    "togglegodmode": "", "togglemenus": "", "togglenavmesh": "", "togglepathgrid": "", "togglerecastmesh": "",
    "togglescripts": "", "togglesky": "", "togglevanitymode": "", "togglewater": "", "togglewireframe": "",
    "toggleworld": "", "tpg": "", "ts": "", "turnmoonred": "", "turnmoonwhite": "", "tvm": "", "tw": "",
    "twa": "", "twf": "", "undowerewolf": "", "unlock": "", "user1": "", "user2": "", "user3": "",
    "user4": "", "wakeuppc": "",
}

_ATTRS = ("strength", "intelligence", "willpower", "agility", "speed", "endurance", "personality", "luck")
_DYN = ("health", "magicka", "fatigue")
_SKILLS = ("block", "armorer", "mediumarmor", "heavyarmor", "bluntweapon", "longblade", "axe", "spear", "athletics",
           "enchant", "destruction", "alteration", "illusion", "conjuration", "mysticism", "restoration", "alchemy",
           "unarmored", "security", "sneak", "acrobatics", "lightarmor", "shortblade", "marksman", "mercantile",
           "speechcraft", "handtohand")


def func_info(name):
    """(result type, signature) of a function OpenMW knows, else None"""
    if name in FUNCS:
        return FUNCS[name]
    if name.startswith("get"):
        stat = name[3:]
        if stat in _ATTRS or stat in _SKILLS:
            return "f", ""
        if stat in _DYN:
            return "f", "x"
        if stat.endswith("getratio") and stat[:-8] in _DYN:
            return "f", ""
    return None


def instr_signature(name):
    """Argument signature of an instruction OpenMW knows, else None"""
    if name in INSTRS:
        return INSTRS[name]
    for prefix in ("modcurrent", "set", "mod"):
        if name.startswith(prefix) and name != prefix:
            stat = name[len(prefix):]
            if stat in _ATTRS or stat in _SKILLS or stat in _DYN:
                return "f"
    return None


class ScriptError(Exception):
    pass


def tokenize(line):
    out, pos = [], 0
    line = line.rstrip()
    while pos < len(line):
        if line[pos].isspace():
            pos += 1
            continue
        m = TOKEN.match(line, pos)
        if not m:
            pos += 1          # stray character (e.g. '[' or '!' typos in vanilla scripts): skip
            continue
        kind = m.lastgroup
        text = m.group(kind)
        if kind == "id2":              # a name that starts with digits (3rd_door); a '-' between name characters is part of it (Ald-ruhn), as in OpenMW's scanner
            kind = "id"
        elif kind == "str":
            text = text.strip('"')
        elif kind == "op" and text in ("[", "]"):
            text = "(" if text == "[" else ")"      # OpenMW reads brackets as parentheses
        out.append((kind, text))
        pos = m.end()
    return out


class Compiler:
    def __init__(self, globals_):
        self.globals = globals_          # lowercase global names (a dict gives their types: 's' 'l' 'f')
        self.locals = []                 # [(type, name)]
        self.leftovers = []              # lines with tokens an expression didn't read (check_rest)
        self.pending_ref = None          # X->( ... ): the reference the next function inside takes

    # ---- types: OpenMW divides whole numbers as whole numbers (int / int), anything with a float as floats ----
    def local_type(self, name):
        for t, n in self.locals:
            if n == name:
                return "l" if t in ("short", "long") else "f"
        return "f"

    def global_type(self, name):
        v = self.globals.get(name) if isinstance(self.globals, dict) else None
        if isinstance(v, (tuple, list)):
            v = v[0]
        return "l" if v in ("s", "l") else "f"

    def ntype(self, a):
        """'l' (whole number) or 'f' (float) of an expression node; what isn't known counts as float"""
        if isinstance(a, int):
            return "l"
        if not isinstance(a, list):
            return "f"
        tag = a[0]
        if tag == "l":
            return self.local_type(a[1])
        if tag == "g":
            return self.global_type(a[1])
        if tag == "c":
            info = func_info(a[2])
            return info[0] if info else "f"
        if tag == "neg":
            return self.ntype(a[1])
        if tag in ("==", "!=", "<", "<=", ">", ">="):
            return "l"
        if tag in ("+", "-", "*", "/", "//"):
            return "l" if self.ntype(a[1]) == "l" and self.ntype(a[2]) == "l" else "f"
        return "f"

    # ---- expressions ----
    def expr(self, toks, i):
        return self.comparison(toks, i)

    def comparison(self, toks, i):
        a, i = self.additive(toks, i)
        while i < len(toks) and toks[i][0] == "op" and toks[i][1] in ("==", "!=", "<>", "<", "<=", ">", ">=", "=", "=<", "=>"):
            # OpenMW's scanner reads =< and => as ==, and <> as <
            op = {"<>": "<", "=": "==", "=<": "==", "=>": "=="}.get(toks[i][1], toks[i][1])
            b, i = self.additive(toks, i + 1)
            a = [op, a, b]
        return a, i

    def additive(self, toks, i):
        a, i = self.term(toks, i)
        while i < len(toks) and toks[i] in (("op", "+"), ("op", "-")):
            op = toks[i][1]
            b, i = self.term(toks, i + 1)
            a = [op, a, b]
        return a, i

    def term(self, toks, i):
        a, i = self.unary(toks, i)
        while i < len(toks) and toks[i] in (("op", "*"), ("op", "/")):
            op = toks[i][1]
            b, i = self.unary(toks, i + 1)
            if op == "/" and self.ntype(a) == "l" and self.ntype(b) == "l":
                op = "//"
            a = [op, a, b]
        return a, i

    def unary(self, toks, i):
        if i < len(toks) and toks[i] == ("op", "-"):
            a, i = self.unary(toks, i + 1)
            return (-a if isinstance(a, (int, float)) else ["neg", a]), i
        if i < len(toks) and toks[i] == ("op", "+"):
            return self.unary(toks, i + 1)          # a unary plus is dropped
        return self.primary(toks, i)

    def check_rest(self, toks, i):
        """Tokens an expression left unread (a function's argument it didn't know of): noted in
        self.leftovers for tools/scriptcheck.py."""
        rest = [t for t in toks[i:] if t not in (("op", ","), ("op", ")"))]
        if rest:
            self.leftovers.append(" ".join(t[1] for t in toks))

    def skip_commas(self, toks, i):
        while i < len(toks) and toks[i] == ("op", ","):
            i += 1
        return i

    def primary(self, toks, i):
        i = self.skip_commas(toks, i)
        if i >= len(toks):
            raise ScriptError("expression expected")
        kind, text = toks[i]
        if kind == "num":
            return (int(text) if text.isdigit() else float(text)), i + 1
        if toks[i] == ("op", "("):
            a, i = self.expr(toks, i + 1)
            i = self.skip_commas(toks, i)
            if i < len(toks) and toks[i] == ("op", ")"):
                i += 1
            return a, i
        if kind in ("str", "id"):
            # explicit reference: X->Func args
            if i + 2 < len(toks) and toks[i + 1] == ("op", "->"):
                # X->( OnDeath == 1 ) (Jeanne's ghost in boneScript): OpenMW keeps the reference over the parenthesis
                # for the first function inside it (ExprParser::parseSpecial)
                if toks[i + 2] == ("op", "("):
                    self.pending_ref = text.lower()
                    a, i = self.expr(toks, i + 3)
                    self.pending_ref = None
                    i = self.skip_commas(toks, i)
                    if i < len(toks) and toks[i] == ("op", ")"):
                        i += 1
                    return a, i
                return self.call(toks, i + 2, text.lower())
            # remote variable: X.var
            if i + 2 < len(toks) and toks[i + 1] == ("op", ".") and toks[i + 2][0] == "id":
                return ["r", text.lower(), toks[i + 2][1].lower()], i + 3
            if kind == "str":
                return ["s", text], i + 1
            name = text.lower()
            if any(n == name for _, n in self.locals):
                return ["l", name], i + 1
            if name in self.globals:
                return ["g", name], i + 1
            ref, self.pending_ref = self.pending_ref, None
            return self.call(toks, i, ref)
        raise ScriptError(f"unexpected {text!r}")

    def call(self, toks, i, ref):
        name = toks[i][1].lower()
        i += 1
        info = func_info(name)
        if info:
            args, i = self.args(toks, i, info[1], True)
            return ["c", ref, name, args], i
        args = []
        for _ in range(EXPR_ARITY.get(name, 0)):
            i = self.skip_commas(toks, i)
            if i >= len(toks) or toks[i] == ("op", ")"):
                break
            a, i = self.arg(toks, i)
            args.append(a)
        return ["c", ref, name, args], i

    def operand_start(self, toks, i):
        """Whether the token can begin an argument"""
        t = toks[i]
        return t[0] != "op" or t[1] in ("(", "-", "+")

    def args(self, toks, i, sig, in_expr):
        """The arguments of a call as OpenMW's signature says: S c x z names are strings (or variables), l f X
        numbers are expressions that end at a comparison or a + or - (OpenMW's ExprParser in argument mode: a
        following -5 is the next argument; inside parentheses anything goes). Where an optional
        argument isn't there (the line or the expression ends) the rest are left out."""
        args = []
        for ch in sig:
            if ch in "/j":
                continue
            i = self.skip_commas(toks, i)
            if i >= len(toks) or not self.operand_start(toks, i) or (ch in "Scxz" and toks[i][0] == "op"):
                break
            if ch in "lfX":
                a, i = self.term(toks, i)
            else:
                a, i = self.arg(toks, i)
                # An id of several words without quotes (HasSoulGem dagoth drals): the words that follow
                while in_expr and isinstance(a, list) and a[0] == "s" and i < len(toks) and toks[i][0] == "id" \
                        and not any(n == toks[i][1].lower() for _, n in self.locals) \
                        and toks[i][1].lower() not in self.globals:
                    a = ["s", a[1] + " " + toks[i][1]]
                    i += 1
            args.append(a)
        return args, i

    def arg(self, toks, i):
        """Function arguments: strings, numbers, variables; bare words are strings (object ids)."""
        kind, text = toks[i]
        if kind == "op" and text == "-" and i + 1 < len(toks) and toks[i + 1][0] == "num":
            n = toks[i + 1][1]
            return (-int(n) if n.isdigit() else -float(n)), i + 2
        if kind == "num":
            return (int(text) if text.isdigit() else float(text)), i + 1
        if kind == "str":
            if i + 2 < len(toks) and toks[i + 1] == ("op", ".") and toks[i + 2][0] == "id":
                return ["r", text.lower(), toks[i + 2][1].lower()], i + 3
            return ["s", text], i + 1
        if kind == "id":
            name = text.lower()
            if any(n == name for _, n in self.locals):
                return ["l", name], i + 1
            if name in self.globals:
                return ["g", name], i + 1
            return ["s", text], i + 1
        return ["s", text], i + 1

    # ---- statements ----
    def message_box(self, toks, i):
        """MessageBox "text %g %s" args..., then the button labels: one argument per placeholder"""
        args = []
        i = self.skip_commas(toks, i)
        if i >= len(toks):
            return args
        a, i = self.arg(toks, i)
        args.append(a)
        fmt = a[1] if isinstance(a, list) and a[0] == "s" else ""
        for m in re.finditer(r"%%|%[.0-9]*([gfdis])", fmt):
            if m.group(0) == "%%":
                continue
            i = self.skip_commas(toks, i)
            if i >= len(toks) or not self.operand_start(toks, i):
                break
            if m.group(1) == "s":
                a, i = self.arg(toks, i)
            else:
                a, i = self.term(toks, i)
            args.append(a)
        while i < len(toks):
            if toks[i] == ("op", ","):
                i += 1
                continue
            a, i = self.arg(toks, i)
            args.append(a)
        return args

    def statement(self, toks):
        kind, text = toks[0]
        word = text.lower() if kind == "id" else None
        if word == "set" and len(toks) >= 4:
            # set <target> to <expr>
            to = next((k for k in range(2, len(toks)) if toks[k][0] == "id" and toks[k][1].lower() == "to"), None)
            if to is None:
                raise ScriptError("set without 'to'")
            target_toks = [t for t in toks[1:to] if t != ("op", ",")]
            if len(target_toks) == 3 and target_toks[1] == ("op", "."):
                target = ["r", target_toks[0][1].lower(), target_toks[2][1].lower()]
            elif len(target_toks) == 3 and target_toks[1] == ("op", "->"):
                target = ["r", target_toks[0][1].lower(), target_toks[2][1].lower()]
            else:
                name = target_toks[0][1].lower()
                target = ["l", name] if any(n == name for _, n in self.locals) else ["g", name]
            value, end = self.expr(toks, to + 1)
            self.check_rest(toks, end)
            return ["set", target, value]
        # call statement: [ref ->] func args...
        i, ref = 0, None
        if len(toks) > 2 and toks[1] == ("op", "->"):
            ref, i = text.lower(), 2
        func = toks[i][1].lower()
        i = self.skip_commas(toks, i + 1)
        if func == "messagebox":
            return ["c", ref, func, self.message_box(toks, i)]
        sig = instr_signature(func)
        if sig is None and func_info(func):
            sig = func_info(func)[1]
        if sig is not None:
            args, i = self.args(toks, i, sig, False)       # what follows the arguments is ignored
            return ["c", ref, func, args]
        args = []
        while i < len(toks):
            if toks[i] == ("op", ","):
                i += 1
                continue
            a, i = self.arg(toks, i)
            args.append(a)
        return ["c", ref, func, args]

    def compile(self, text):
        """Returns {"name", "locals": [[type, name]], "body": [...]}."""
        lines = []
        for raw in text.replace("\r", "").split("\n"):
            code = raw.split(";", 1)[0] if '"' not in raw.split(";", 1)[0] or raw.count('"') % 2 == 0 else raw
            # keep ';' inside strings: only cut at a ';' outside quotes
            code, quoted = "", False
            for ch in raw:
                if ch == '"':
                    quoted = not quoted
                if ch == ";" and not quoted:
                    break
                code += ch
            toks = tokenize(code)
            while toks and toks[0][0] == "op":       # stray special characters at the start of a line are skipped
                toks.pop(0)
            if toks:
                lines.append(toks)

        name, root = "", []
        stack = [("root", root)]          # frames: (kind, list or if-node)
        # Morrowind's own compiler takes a few malformed blocks (Tarhiel's fallingScript, HentusTravel,
        # vedeleaFollow, BILL_synette_jeline, VampireMolag shipped with them): a stray endif is dropped, an
        # elseif / else after an endif closed its chain too early reopens that chain, and a second else
        # is dead code. (Rejecting them silently dropped the whole script.)
        last_closed = None                # (stack depth, frame) of the if-chain the last endif closed
        for toks in lines:
            kind, text = toks[0]
            word = text.lower() if kind == "id" else None
            if word in ("elseif", "else") and stack[-1][0] != "if" and last_closed and last_closed[0] == len(stack):
                stack.append(last_closed[1])
                last_closed = None
            body = stack[-1][1] if stack[-1][0] in ("root", "while", "dead") else stack[-1][1][1][-1][1] \
                if stack[-1][0] == "if" and not stack[-1][2] else stack[-1][1][2] if stack[-1][0] == "if" else None
            if word == "begin":
                name = toks[1][1] if len(toks) > 1 else ""
            elif word == "end":
                break
            elif word in ("short", "long", "float"):
                for t in toks[1:]:
                    if t[0] == "id":
                        self.locals.append((word, t[1].lower()))
            elif word == "if":
                cond, end = self.expr(toks, 1)
                self.check_rest(toks, end)
                node = ["if", [[cond, []]], None]
                body.append(node)
                stack.append(("if", node, False))
            elif word == "elseif":
                cond, end = self.expr(toks, 1)
                self.check_rest(toks, end)
                if stack[-1][0] != "if":              # OpenMW starts an if here (a warning)
                    node = ["if", [[cond, []]], None]
                    body.append(node)
                    stack.append(("if", node, False))
                else:
                    stack[-1][1][1].append([cond, []])
            elif word == "else":
                if stack[-1][0] != "if":
                    continue                          # a stray else: ignored (OpenMW warns)
                if stack[-1][2]:
                    stack.pop()
                    stack.append(("dead", []))    # a second else: never runs
                    continue
                stack[-1][1][2] = []
                stack[-1] = ("if", stack[-1][1], True)
            elif word == "endif":
                if stack[-1][0] == "dead":
                    stack.pop()
                    continue
                if stack[-1][0] != "if":
                    continue                      # a stray endif
                last_closed = (len(stack) - 1, stack.pop())
            elif word == "while":
                cond, _ = self.expr(toks, 1)
                node = ["while", cond, []]
                body.append(node)
                stack.append(("while", node[2]))
            elif word == "endwhile":
                if stack[-1][0] != "while":
                    raise ScriptError("endwhile without while")
                stack.pop()
            elif word == "return":
                body.append(["ret"])
            else:
                body.append(self.statement(toks))
        return {"name": name, "locals": [[t, n] for t, n in self.locals], "body": root}


def compile_script(text, globals_):
    return Compiler(globals_).compile(text)


def compile_snippet(text, globals_, local_names=()):
    """Dialogue result scripts: no begin/end, run in the speaker's context."""
    c = Compiler(globals_)
    c.locals = [("unknown", n) for n in local_names]      # the speaker's variables: types not known here
    return c.compile(text)["body"]
