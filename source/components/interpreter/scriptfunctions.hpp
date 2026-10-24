#pragma once

// The script functions the interpreter knows: id (F_<NAME>) and lower-case name, and the stat names the Get / Set /
// Mod functions take. tools/convert/mwscript.py (engine_functions) reads this file to know what the engine runs:
// keep the X(...) and table layout

#define SCRIPT_FUNCTIONS(X) \
	X(UNKNOWN, "") X(MENUMODE, "menumode") X(ONACTIVATE, "onactivate") X(ACTIVATE, "activate") \
	X(GETHEALTH, "gethealth") X(SETHELLO, "sethello") X(ENABLE, "enable") X(DISABLE, "disable") \
	X(GETDISABLED, "getdisabled") X(GETDISTANCE, "getdistance") X(SAY, "say") X(SAYDONE, "saydone") \
	X(DISABLEPLAYERCONTROLS, "disableplayercontrols") X(ENABLEPLAYERCONTROLS, "enableplayercontrols") \
	X(DISABLEPLAYERJUMPING, "disableplayerjumping") X(ENABLEPLAYERJUMPING, "enableplayerjumping") \
	X(ENABLEPLAYERFIGHTING, "enableplayerfighting") X(DISABLEPLAYERFIGHTING, "disableplayerfighting") \
	X(ENABLEPLAYERMAGIC, "enableplayermagic") X(DISABLEPLAYERMAGIC, "disableplayermagic") \
	X(ENABLEPLAYERVIEWSWITCH, "enableplayerviewswitch") X(DISABLEPLAYERVIEWSWITCH, "disableplayerviewswitch") \
	X(ENABLEVANITYMODE, "enablevanitymode") X(DISABLEVANITYMODE, "disablevanitymode") \
	X(ENABLECLASSMENU, "enableclassmenu") X(ENABLEBIRTHMENU, "enablebirthmenu") \
	X(ENABLESTATREVIEWMENU, "enablestatreviewmenu") X(ENABLERACEMENU, "enableracemenu") \
	X(ENABLENAMEMENU, "enablenamemenu") X(ENABLESTATSMENU, "enablestatsmenu") \
	X(ENABLEINVENTORYMENU, "enableinventorymenu") X(ENABLEMAGICMENU, "enablemagicmenu") \
	X(ENABLEMAPMENU, "enablemapmenu") X(ENABLEREST, "enablerest") X(SHOWRESTMENU, "showrestmenu") \
	X(STARTSCRIPT, "startscript") X(STOPSCRIPT, "stopscript") X(SCRIPTRUNNING, "scriptrunning") \
	X(MESSAGEBOX, "messagebox") X(GETBUTTONPRESSED, "getbuttonpressed") X(XBOX, "xbox") \
	X(GETSECONDSPASSED, "getsecondspassed") X(GETITEMCOUNT, "getitemcount") X(ADDITEM, "additem") \
	X(REMOVEITEM, "removeitem") X(UNLOCK, "unlock") X(LOCK, "lock") X(GETLOCKED, "getlocked") \
	X(PLAYSOUND, "playsound") X(PLAYSOUND3D, "playsound3d") X(PLAYSOUNDVP, "playsoundvp") \
	X(PLAYSOUND3DVP, "playsound3dvp") X(GETSTANDINGPC, "getstandingpc") X(GETRACE, "getrace") \
	X(JOURNAL, "journal") X(GETJOURNALINDEX, "getjournalindex") X(SETJOURNALINDEX, "setjournalindex") \
	X(ADDTOPIC, "addtopic") X(GOODBYE, "goodbye") X(CHOICE, "choice") X(FORCEGREETING, "forcegreeting") \
	X(MODDISPOSITION, "moddisposition") X(SETDISPOSITION, "setdisposition") X(GETDISPOSITION, "getdisposition") \
	X(RANDOM, "random") X(GETPCCELL, "getpccell") X(POSITIONCELL, "positioncell") X(CHANGEWEATHER, "changeweather") \
	X(SHOWMAP, "showmap") X(CLEARINFOACTOR, "clearinfoactor") X(EQUIP, "equip") X(HASITEMEQUIPPED, "hasitemequipped") \
	X(GETPOS, "getpos") X(GETANGLE, "getangle") X(ADDSPELL, "addspell") X(REMOVESPELL, "removespell") \
	X(GETSPELL, "getspell") X(MODPCFACREP, "modpcfacrep") X(STREAMMUSIC, "streammusic") X(FADEIN, "fadein") \
	X(FADEOUT, "fadeout") X(STOPSOUND, "stopsound") X(GETSOUNDPLAYING, "getsoundplaying") \
	X(GETCURRENTTIME, "getcurrenttime") X(PCJOINFACTION, "pcjoinfaction") X(GETPCRANK, "getpcrank") \
	X(GETPCCRIMELEVEL, "getpccrimelevel") X(SETPCCRIMELEVEL, "setpccrimelevel") X(PAYFINE, "payfine") \
	X(PAYFINETHIEF, "payfinethief") X(ONDEATH, "ondeath") X(GETDEADCOUNT, "getdeadcount") X(STARTCOMBAT, "startcombat") X(STOPCOMBAT, "stopcombat") \
	X(PCRAISERANK, "pcraiserank") X(PCLOWERRANK, "pclowerrank") X(PCEXPELLED, "pcexpelled") \
	X(GETPCFACREP, "getpcfacrep") X(SETPCFACREP, "setpcfacrep") X(MODREPUTATION, "modreputation") \
	X(SETREPUTATION, "setreputation") X(GETREPUTATION, "getreputation") X(MODFIGHT, "modfight") \
	X(SETFIGHT, "setfight") X(GETFIGHT, "getfight") X(GETPCSLEEP, "getpcsleep") X(WAKEUPPC, "wakeuppc") \
	X(PCEXPELL, "pcexpell") X(PCCLEAREXPELLED, "pcclearexpelled") X(CELLCHANGED, "cellchanged") \
	X(MODFACTIONREACTION, "modfactionreaction") X(SETFACTIONREACTION, "setfactionreaction") \
	X(GETFACTIONREACTION, "getfactionreaction") X(DONTSAVEOBJECT, "dontsaveobject") X(PLAYBINK, "playbink") \
	X(ENABLETELEPORTING, "enableteleporting") X(DISABLETELEPORTING, "disableteleporting") \
	X(AIWANDER, "aiwander") X(AITRAVEL, "aitravel") X(AIFOLLOW, "aifollow") X(AIFOLLOWCELL, "aifollowcell") \
	X(AIESCORT, "aiescort") X(AIESCORTCELL, "aiescortcell") X(AIACTIVATE, "aiactivate") \
	X(GETAIPACKAGEDONE, "getaipackagedone") X(GETCURRENTAIPACKAGE, "getcurrentaipackage") \
	X(ONMURDER, "onmurder") X(GETCOMMONDISEASE, "getcommondisease") X(GETBLIGHTDISEASE, "getblightdisease") \
	X(CAST, "cast") X(PLAYGROUP, "playgroup") X(LOOPGROUP, "loopgroup") X(ROTATE, "rotate") \
	X(PLAYLOOPSOUND3D, "playloopsound3d") X(PLAYLOOPSOUND3DVP, "playloopsound3dvp") X(MODREGION, "modregion") \
	X(PLACEATPC, "placeatpc") X(PLACEATME, "placeatme") X(POSITION, "position") \
	X(HITONME, "hitonme") X(GETEFFECT, "geteffect") X(GETSPELLEFFECTS, "getspelleffects") X(GETTARGET, "gettarget") \
	X(GETDETECTED, "getdetected") X(GETLOS, "getlos") X(GETLINEOFSIGHT, "getlineofsight") X(GETATTACKED, "getattacked") \
	X(SETANGLE, "setangle") X(SETPOS, "setpos") X(MOVE, "move") X(MOVEWORLD, "moveworld") X(ROTATEWORLD, "rotateworld") \
	X(SETATSTART, "setatstart") X(GETSTARTINGANGLE, "getstartingangle") X(GETSTARTINGPOS, "getstartingpos") \
	X(ONKNOCKOUT, "onknockout") X(RESURRECT, "resurrect") X(MODFLEE, "modflee") X(SETFLEE, "setflee") \
	X(GETFLEE, "getflee") X(SETALARM, "setalarm") X(GETALARM, "getalarm") X(MODALARM, "modalarm") X(DROP, "drop") \
	X(FORCESNEAK, "forcesneak") X(CLEARFORCESNEAK, "clearforcesneak") X(HURTSTANDINGACTOR, "hurtstandingactor") \
	X(GETFORCESNEAK, "getforcesneak") X(FORCERUN, "forcerun") X(CLEARFORCERUN, "clearforcerun") \
	X(GETFORCERUN, "getforcerun") X(FORCEJUMP, "forcejump") X(CLEARFORCEJUMP, "clearforcejump") \
	X(GETFORCEJUMP, "getforcejump") X(FORCEMOVEJUMP, "forcemovejump") X(CLEARFORCEMOVEJUMP, "clearforcemovejump") \
	X(GETFORCEMOVEJUMP, "getforcemovejump") X(ENABLEPLAYERLOOKING, "enableplayerlooking") \
	X(DISABLEPLAYERLOOKING, "disableplayerlooking") X(GETPLAYERCONTROLSDISABLED, "getplayercontrolsdisabled") \
	X(GETPLAYERFIGHTINGDISABLED, "getplayerfightingdisabled") X(GETPLAYERJUMPINGDISABLED, "getplayerjumpingdisabled") \
	X(GETPLAYERLOOKINGDISABLED, "getplayerlookingdisabled") X(GETPLAYERMAGICDISABLED, "getplayermagicdisabled") \
	X(GETPLAYERVIEWSWITCHDISABLED, "getplayerviewswitchdisabled") X(GETVANITYMODEDISABLED, "getvanitymodedisabled") \
	X(GOTOJAIL, "gotojail") X(RAISERANK, "raiserank") X(LOWERRANK, "lowerrank") X(HASSOULGEM, "hassoulgem") \
	X(REMOVESOULGEM, "removesoulgem") X(GETCURRENTWEATHER, "getcurrentweather") X(GETPOS_, "getposition") X(FALL, "fall") 	X(COC, "coc") X(CENTERONCELL, "centeroncell") X(COE, "coe") X(CENTERONEXTERIOR, "centeronexterior") 	X(TGM, "tgm") X(TOGGLEGODMODE, "togglegodmode") X(TAI, "tai") X(TOGGLEAI, "toggleai") X(SV, "sv") 	X(SHOWVARS, "showvars") X(ORI, "ori") X(OUTPUTREFINFO, "outputrefinfo") X(BC, "bc") X(BETACOMMENT, "betacomment") 	X(HELP, "help") X(FILLJOURNAL, "filljournal") X(FILLMAP, "fillmap") \
	X(GETINTERIOR, "getinterior") X(GETSQUAREROOT, "getsquareroot") X(GETSTAT, "getstat") \
	X(GETPCSNEAKING, "getpcsneaking") X(GETWEAPONDRAWN, "getweapondrawn") X(GETCHAMELEON, "getchameleon") \
	X(GETPARALYSIS, "getparalysis") X(FACE, "face") X(SETDELETE, "setdelete") \
	X(ENABLELEVITATION, "enablelevitation") X(DISABLELEVITATION, "disablelevitation") \
	X(GETSTANDINGACTOR, "getstandingactor") X(GETCOLLIDINGPC, "getcollidingpc") \
	X(GETCOLLIDINGACTOR, "getcollidingactor") X(HURTCOLLIDINGACTOR, "hurtcollidingactor") X(EXPLODESPELL, "explodespell") \
	X(GETWATERLEVEL, "getwaterlevel") X(SETWATERLEVEL, "setwaterlevel") X(MODWATERLEVEL, "modwaterlevel") \
	X(PLACEITEM, "placeitem") X(PLACEITEMCELL, "placeitemcell")

enum ScriptFunction
{
#define X(id, name) F_##id,
	SCRIPT_FUNCTIONS(X)
#undef X
	F_COUNT
};

static const char* const kFunctionNames[] = {
#define X(id, name) name,
	SCRIPT_FUNCTIONS(X)
#undef X
};

static const char* const kAttrFuncs[8] = { "strength", "intelligence", "willpower", "agility", "speed", "endurance",
	"personality", "luck" };
static const char* const kSkillFuncs[27] = { "block", "armorer", "mediumarmor", "heavyarmor", "bluntweapon", "longblade",
	"axe", "spear", "athletics", "enchant", "destruction", "alteration", "illusion", "conjuration", "mysticism",
	"restoration", "alchemy", "unarmored", "security", "sneak", "acrobatics", "lightarmor", "shortblade", "marksman",
	"mercantile", "speechcraft", "handtohand" };
