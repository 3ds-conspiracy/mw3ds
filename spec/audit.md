# Audit: which of our rules have been checked against OpenMW

Started 2026-10-01 after two device bugs (NPCs not aimable after leaving the Census office; the "cave rat"
topic never learned) traced to code written early from how Morrowind looks, never compared with OpenMW, and
tests that skipped the player's path. OpenMW's game code is in `build/openmw-full/` (sparse clone of
`apps/openmw/{mwscript,mwdialogue,mwworld,mwmechanics,mwgui,mwphysics,mwclass}` and
`components/{compiler,interpreter}`; ignored by git; `git -C build/openmw-full pull` to update).

Status: **checked** = compared line by line, spec page and test; **partial** = the numbers are checked, the
logic around them isn't; **unchecked** = never compared; **differs** = on purpose, reason given;
**n/a** = 3DS engine code with no OpenMW counterpart.

Order of work: script functions, dialogue, world, movement and physics, rest, the menus' rules.
For each unchecked rule: read the OpenMW code, write what it does into a spec page, compare, write a test that
goes the way a player goes (no direct calls past the crosshair or the topic list), fix.

## By area

| Our code | Rule | OpenMW | Status |
|---|---|---|---|
| `script.cpp` | 173 script functions (table below) | `mwscript/*extensions.cpp`, `components/interpreter` | unchecked |
| `script.cpp` | expression evaluation, `if` / `while`, locals, `return`, GetSecondsPassed, targets (`x->`) | `components/compiler`, `components/interpreter` | unchecked |
| `world.cpp` | local / global scripts: which run, when, StartScript / StopScript | `mwscript/globalscripts.cpp`, `mwworld/localscripts.cpp` | unchecked |
| `dialogue.cpp` | which response: filter order and each condition (`passes`, `filterFunction`) | `mwdialogue/filter.cpp` | unchecked |
| `dialogue.cpp` | topics named in text become known; links on screen | `mwdialogue/keywordsearch.cpp`, `dialoguemanagerimp.cpp addTopicsFromText` | checked 2026-10-01 (`fg-rathunt`) |
| `dialogue.cpp` | the topic list; exhausted topics (greyed) | `dialoguemanagerimp.cpp updateActorKnownTopics`, `mwgui/dialogue.cpp` | partial: exhausted not done |
| `dialogue.cpp` | greetings (Greeting 0-9), choices, Goodbye, result scripts | `dialoguemanagerimp.cpp startDialogue / questionAnswered / executeScript` | unchecked |
| `dialogue.cpp` | text substitution (%PCName, %PCRank ...) | `components/interpreter/defines.cpp` | unchecked |
| `dialogue.cpp` | voiced reactions (Hello, Attack, Hit, Flee, Thief ...) | `dialoguemanagerimp.cpp say`, `mwmechanics/actors.cpp` | partial (greetings in `npc-ai-behaviour.md`) |
| `world.cpp` | disposition, faction reaction | `mechanicsmanagerimp.cpp getDerivedDisposition` | unchecked (persuasion checked) |
| `world.cpp` | containers and NPC inventories filled, leveled lists, restock | `mwworld/containerstore.cpp fill / restock`, `mwmechanics/levelledlist.cpp` | unchecked |
| `world.cpp` | corpses and containers respawning | `mwworld/cellstore.cpp respawn` | unchecked |
| `world.cpp` | weather, daylight | `mwworld/weather.cpp` | unchecked |
| `world.cpp` | owned and stolen items | `mechanicsmanagerimp.cpp isAllowedToUse / itemTaken`, `mwworld/cellref.cpp` | partial (`crime.md`) |
| `combat.cpp` | arrest: the guard comes over and talks, Greeting 0 decides (pay, jail, resist, the character creation pardon) | `aipursue.cpp`, `actors.cpp` (Alarmed, crime id), `miscextensions.cpp OpPayFine / OpGoToJail` | checked 2026-10-07 (`crime.md`, `issue-4`, `crime-arrest-*`) |
| `combat.cpp` | guards go for hostile creatures: a creature in a fight within fAlarmRadius, Fight towards the player 100 or more, not calmed, not following or escorting anyone (`guardFoe`, then the duel) | `actors.cpp engageCombat` | checked 2026-10-12 (`bug-guard-creature`); differs: the creature keeps on the player rather than turning on the nearest foe |
| `world.cpp` | followers taken through doors | `mwworld/actionteleport.cpp getFollowers` | unchecked |
| `world.cpp` | enable / disable, pick up, add / remove items, stacks | `mwworld/worldimp.cpp`, `containerstore.cpp`, `actiontake.cpp` | unchecked |
| `world.cpp` | soul trap | `mwmechanics/spelleffects.cpp` (soultrap), `actors.cpp` | unchecked |
| `screens_character.cpp` | journal: topic links in entries and topic pages | `mwgui/journalbooks.cpp`, `journalviewmodel.cpp` | checked 2026-10-07 (`dialogue.md` R25, `issue-26`) |
| `screens_map.cpp` | local map indoors and out, door markers and the name they show | `mwgui/mapwindow.cpp`, `mwworld/worldimp.cpp` (door markers), `mwclass/door.cpp getDestination` | checked 2026-10-07 (`issue-23`, `issue-24`): the marker names the destination cell, as OpenMW; a tap shows it (OpenMW: hover), the buttons and sizes differ (3DS UI, kept) |
| `world.cpp` | journal entries, quest finished | `mwdialogue/journalimp.cpp`, `quest.cpp` | unchecked |
| `world.cpp` | date and time, day / month rollover | `mwworld/datetimemanager.cpp` | unchecked |
| `world.cpp` | what the crosshair picks | `mwworld/worldimp.cpp getFacedObject` (rays the drawn scene) | differs: boxes (cost); boxes now follow their objects; an item inside a body's box is picked before the body (`issue-13`) |
| `world.cpp` `save.cpp` | objects leaving memory and coming back; saves | (no counterpart: OpenMW keeps cell state) | n/a, guarded by the position monitors |
| `session.cpp` | activation: doors, locked doors and keys, containers, traps, books, pickpocket | `mwclass/door.cpp`, `container.cpp`, `npc.cpp activate`, `mwworld/action*.cpp` | unchecked |
| `session.cpp` | barter, training, travel, spells, repair prices | `mwgui/tradewindow.cpp`, `mwmechanics/trading.cpp` ... | checked (`trading-and-services.md`) |
| `spellmake.cpp` `enchant.cpp` | naming the made spell / item, Buy refusals | `mwgui/spellcreationdialog.cpp`, `mwgui/enchantingdialog.cpp` (`onBuyButtonClicked`, `setItem`) | checked 2026-10-07 (`trading-and-services.md`, `issue-29`) |
| `session.cpp` `combat.cpp` | combat numbers | `mwmechanics/combat.cpp` ... | checked (`combat.md`); AI choices around them partial |
| `magic.cpp` | cast chance, cost, resist, reflect, absorb | `spellcasting.cpp`, `spelleffects.cpp` | checked (`magic.md`); the 134 effects one by one unchecked |
| `rest.cpp` | skill gain, level-up | `npcstats.cpp` | checked (`stats-and-levelling.md`) |
| `rest.cpp` | skill books | `mwworld/actionread.cpp` | unchecked |
| `rest.cpp` | resting: who may rest, interruptions, regeneration per hour | `mwgui/waitdialog.cpp`, `mechanicsmanagerimp.cpp rest`, `mwmechanics/actors.cpp` | partial (regeneration numbers in `stats-and-levelling.md`) |
| `player.cpp` `collision.cpp` | walking, steps, slopes, swimming, falling, jumps, levitation | `mwphysics/movementsolver.cpp`, `physicssystem.cpp`, `mwmechanics/character.cpp` | partial (speeds, jump momentum, levitation, the player's 46 degree walkable slope in `movement.md`; sliding inertia, actors' slope and Slow Fall open) |
| `pathfind.cpp` | NPC routes, path grids | `mwmechanics/pathfinding.cpp`, `pathgrid.cpp`, `aipackage.cpp` | unchecked |
| `renderer.cpp` | enchanted items' shimmer (held, NPCs' weapons, lying about, dropped, in flight) | `components/sceneutil` `addEnchantedGlow`, `projectilemanager.cpp` | differs: the frames follow the item's own texture coordinates (OpenMW: the view), 20 a second; checked by eye (`issue-17` shots) |
| `combat.cpp` | getting unstuck (`npcMoveTo`) | `mwmechanics/obstacle.cpp` | differs: OpenMW's evasion order kept, an opening looked for first (no navigation mesh here; `npc-ai-behaviour.md`, `issue-20`) |
| `screens_items.cpp` | equipping (what can be worn, two-handed and shields, races that can't wear boots / helmets) | `mwworld/inventorystore.cpp`, `actionequip.cpp`, `mwclass/*.cpp canBeEquipped` | unchecked |
| `screens_items.cpp` | taking from containers, bodies, pickpocketing | `mwgui/container.cpp`, `mwmechanics/pickpocket.cpp` | partial (pickpocket chance) |
| `screens_items.cpp` | quick keys | `mwgui/quickkeysmenu.cpp` | unchecked |
| `screens_character.cpp` | class quiz, generated class, birthsign and race effects at creation | `mwgui/class.cpp`, `mwmechanics/mechanicsmanagerimp.cpp` (buildPlayer) | partial (creation stats in `stats-and-levelling.md`) |
| `actors.cpp` `audio.cpp` | which animation / sound plays | `mwmechanics/character.cpp`, `mwsound` | partial: footstep sounds (`movement.md`), hit visuals (`magic.md`); hits and deaths one of Hit1..5 / Death1..5 at random, DeathKnockDown / DeathKnockOut from the floor (`chooseRandomGroup`, `playRandomDeath`), a creature's Attack1..3 at random with that attack's damage (`chooseRandomAttackAnimation`, `Creature::hit`), a weapon's chop / slash / thrust by `chooseBestAttack` played as its own group, the weapon stance's idle / run in combat; the player's third-person body walks, runs and sneaks forward / back / left / right and jumps (`bug-anim-groups`, `bug-body-moves`); poses of every NPC, weapon, ranged and creature group checked against OpenMW shots (`tools/test/sidebyside.py`); differs: NPCs don't strafe or back off (their AI doesn't), so they only walk forward; no upper-body layer |
| `combat.cpp` `world.cpp` | where a creature stands: its model's "Bounding Box" rested on the highest ground under it, turned with it unless square and centred | `mwphysics/actor.cpp` (box, `RotatingBox`), `movementsolver.cpp traceDown`, `components/nifbullet` `findBoundingBox` | partial: `bug-creature-slope`; sampled at 9 points of the box, not swept; people keep the narrow footprint (their 58-unit box would perch them on counters; OpenMW's sideways collision keeps them off) |
| `actors.cpp` `creature.py` | a creature model's NiVisController (a Bonelord's robe goes as it dies, a Flame Atronach's fists burn as it strikes) on its animation time; its particle systems as crossed quads riding the emitting node | `nifosg` `VisController`, `mwrender/animation.cpp` | partial: `bug-bonelord-robe`, `bug-atronach-fire`; particles don't emit, move or fade |
| `viewmodel.cpp` `combat.cpp` | the arrow on the bow from "shoot attach" to "shoot release", the bolt in the crossbow from its drawing, gone at "shoot release", back at "shoot follow attach"; on the weapon's ArrowBone, following its keyframes | `mwrender/weaponanimation.cpp` `attachArrow`, `npcanimation.cpp getArrowBone`, `mwmechanics/character.cpp handleTextKey` | partial: `bug-fp-ammo` (first person); not yet on the third-person body or NPC archers |
| `viewmodel.cpp` | first-person arms: the eye at the first-person skeleton's Camera bone, the view along the player's yaw and pitch (the bone's own turn ignored, so a head nod in a cast brings the hands into view) | `mwrender/camera.cpp` (first person tracks the "Camera" node), `npcanimation.cpp` | partial: spell casts checked against OpenMW shots (`bug-fp-cast-arms`, `tools/test/sidebyside.py`); pitch unchecked (OpenMW turns `bip01 neck` by 0.75 of it, we turn the whole rig) |
| `projectile.cpp`, `magic.cpp` | spell visuals: the cast's on its caster's feet facing as it faces (player and NPCs), the hit's on the target, the area burst at twice the area in feet, a bolt as its effect's model or `VFX_DefaultBolt`, the `VFX_Default*` statics for effects naming none; their node keyframes played (sampled by `firstperson.py`), billboards turned to the eye | `mwmechanics/spellcasting.cpp` (`playSpellCastingEffects`, `explodeSpell`, `playEffects`), `mwworld/projectilemanager.cpp`, `mwrender/animation.cpp` `addEffect` | partial: `spell-vfx-cast`, `spell-vfx-area`, `spell-vfx-npc-cast`, `spell-vfx-default-bolt`, shots against OpenMW (`tools/test/sidebyside.py`); differs: particles are fixed quads riding their emitter (no emission), no alpha / colour controllers, no particle texture override (`PTEX`), no light on bolts; at most 12 at once within 4096 units of the player (3DS cost) |
| `renderer.cpp` `cell.cpp` `distant.cpp` `linear.cpp` `zfile.cpp` `log.cpp` `ui.cpp` `testdrive.cpp` `main.cpp` `screenshot.cpp` | | | n/a |

## Script functions by OpenMW file

All **unchecked**. Generated by `build/audit_script_map.py` (our names from `script.cpp`, OpenMW's from
`components/compiler/extensions0.cpp`, implementations from `mwscript/*extensions.cpp`). Not in this table, they
are registered by OpenMW in loops: the player-control switches (`controlextensions.cpp`), GetHealth and the
other dynamic stats (`statsextensions.cpp`), MessageBox (`guiextensions.cpp`), GetPosition
(`transformationextensions.cpp`).

| OpenMW file | Count | Functions (OpenMW class) |
|---|---|---|
| `miscextensions.cpp` | 33 | activate (`OpActivate`), cast (`OpCast`), disableteleporting (`OpEnableTeleporting`), dontsaveobject (`OpDontSaveObject`), drop (`OpDrop`), enableteleporting (`OpEnableTeleporting`), fadein (`OpFadeIn`), fadeout (`OpFadeOut`), getattacked (`OpGetAttacked`), getcurrenttime (`OpGetCurrentTime`), geteffect (`OpGetEffect`), getlocked (`OpGetLocked`), getpcsleep (`OpGetPcSleep`), getsecondspassed (`OpGetSecondsPassed`), getspelleffects (`OpGetSpellEffects`), getstandingpc (`OpGetStandingPc`), gotojail (`OpGoToJail`), hitonme (`OpHitOnMe`), hurtstandingactor (`OpHurtStandingActor`), lock (`OpLock`), menumode (`OpMenuMode`), onactivate (`OpOnActivate`), payfine (`OpPayFine`), payfinethief (`OpPayFineThief`), playbink (`OpPlayBink`), random (`OpRandom`), removesoulgem (`OpRemoveSoulGem`), scriptrunning (`OpScriptRunning`), startscript (`OpStartScript`), stopscript (`OpStopScript`), unlock (`OpUnlock`), wakeuppc (`OpWakeUpPc`), xbox (`OpXBox`) |
| `statsextensions.cpp` | 28 | addspell (`OpAddSpell`), getblightdisease (`OpGetBlightDisease`), getcommondisease (`OpGetCommonDisease`), getdeadcount (`OpGetDeadCount`), getdisposition (`OpGetDisposition`), getpccrimelevel (`OpGetPCCrimeLevel`), getpcfacrep (`OpGetPCFacRep`), getpcrank (`OpGetPCRank`), getrace (`OpGetRace`), getspell (`OpGetSpell`), lowerrank (`OpLowerRank`), moddisposition (`OpModDisposition`), modpcfacrep (`OpModPCFacRep`), ondeath (`OpOnDeath`), onknockout (`OpOnKnockout`), onmurder (`OpOnMurder`), pcclearexpelled (`OpPcClearExpelled`), pcexpell (`OpPcExpell`), pcexpelled (`OpPcExpelled`), pcjoinfaction (`OpPCJoinFaction`), pclowerrank (`OpPCLowerRank`), pcraiserank (`OpPCRaiseRank`), raiserank (`OpRaiseRank`), removespell (`OpRemoveSpell`), resurrect (`OpResurrect`), setdisposition (`OpSetDisposition`), setpccrimelevel (`OpSetPCCrimeLevel`), setpcfacrep (`OpSetPCFacRep`) |
| `aiextensions.cpp` | 25 | aiactivate (`OpAiActivate`), aiescort (`OpAiEscort`), aiescortcell (`OpAiEscortCell`), aifollow (`OpAiFollow`), aifollowcell (`OpAiFollowCell`), aitravel (`OpAiTravel`), aiwander (`OpAiWander`), getaipackagedone (`OpGetAiPackageDone`), getalarm (`OpGetAiSetting`), getcurrentaipackage (`OpGetCurrentAIPackage`), getdetected (`OpGetDetected`), getfight (`OpGetAiSetting`), getflee (`OpGetAiSetting`), getlineofsight (`OpGetLineOfSight`), getlos (`OpGetLineOfSight`), gettarget (`OpGetTarget`), modalarm (`OpModAiSetting`), modfight (`OpModAiSetting`), modflee (`OpModAiSetting`), setalarm (`OpSetAiSetting`), setfight (`OpSetAiSetting`), setflee (`OpSetAiSetting`), sethello (`OpSetAiSetting`), startcombat (`OpStartCombat`), stopcombat (`OpStopCombat`) |
| `transformationextensions.cpp` | 16 | getangle (`OpGetAngle`), getdistance (`OpGetDistance`), getpos (`OpGetPos`), getstartingangle (`OpGetStartingAngle`), getstartingpos (`OpGetStartingPos`), move (`OpMove`), moveworld (`OpMoveWorld`), placeatme (`OpPlaceAt`), placeatpc (`OpPlaceAt`), position (`OpPosition`), positioncell (`OpPositionCell`), rotate (`OpRotate`), rotateworld (`OpRotateWorld`), setangle (`OpSetAngle`), setatstart (`OpSetAtStart`), setpos (`OpSetPos`) |
| `dialogueextensions.cpp` | 14 | addtopic (`OpAddTopic`), choice (`OpChoice`), clearinfoactor (`OpClearInfoActor`), forcegreeting (`OpForceGreeting`), getfactionreaction (`OpGetFactionReaction`), getjournalindex (`OpGetJournalIndex`), getreputation (`OpGetReputation`), goodbye (`OpGoodbye`), journal (`OpJournal`), modfactionreaction (`OpModFactionReaction`), modreputation (`OpModReputation`), setfactionreaction (`OpSetFactionReaction`), setjournalindex (`OpSetJournalIndex`), setreputation (`OpSetReputation`) |
| `guiextensions.cpp` | 13 | enablebirthmenu (`OpShowDialogue`), enableclassmenu (`OpShowDialogue`), enableinventorymenu (`OpEnableWindow`), enablemagicmenu (`OpEnableWindow`), enablemapmenu (`OpEnableWindow`), enablenamemenu (`OpShowDialogue`), enableracemenu (`OpShowDialogue`), enablerest (`OpEnableRest`), enablestatreviewmenu (`OpShowDialogue`), enablestatsmenu (`OpEnableWindow`), getbuttonpressed (`OpGetButtonPressed`), showmap (`OpShowMap`), showrestmenu (`OpShowRestMenu`) |
| `soundextensions.cpp` | 11 | getsoundplaying (`OpGetSoundPlaying`), playloopsound3d (`OpPlaySound3D`), playloopsound3dvp (`OpPlaySoundVP3D`), playsound (`OpPlaySound`), playsound3d (`OpPlaySound3D`), playsound3dvp (`OpPlaySoundVP3D`), playsoundvp (`OpPlaySoundVP`), say (`OpSay`), saydone (`OpSayDone`), stopsound (`OpStopSound`), streammusic (`OpStreamMusic`) |
| `containerextensions.cpp` | 6 | additem (`OpAddItem`), equip (`OpEquip`), getitemcount (`OpGetItemCount`), hasitemequipped (`OpHasItemEquipped`), hassoulgem (`OpHasSoulGem`), removeitem (`OpRemoveItem`) |
| `controlextensions.cpp` | 5 | clearforcesneak (`OpClearMovementFlag`), disable (`OpSetControl`), enable (`OpSetControl`), forcesneak (`OpSetMovementFlag`), getdisabled (`OpGetDisabled`) |
| `skyextensions.cpp` | 3 | changeweather (`OpChangeWeather`), getcurrentweather (`OpGetCurrentWeather`), modregion (`OpModRegion`) |
| `cellextensions.cpp` | 2 | cellchanged (`OpCellChanged`), getpccell (`OpGetPCCell`) |
| `animationextensions.cpp` | 2 | loopgroup (`OpLoopAnim`), playgroup (`OpPlayAnim`) |
