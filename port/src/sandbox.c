/**
 * sandbox.c - Sandbox mode (port/include/sandbox.h).
 *
 * Two halves:
 *
 *   SETTINGS. The difficulty screen (front.c) shows three toggles under the
 *   difficulties: Empty level, Objectives and Level exits. Starting a mission
 *   from that screen arms them, and lvlStageLoad latches them for that one
 *   mission, so multiplayer and direct launches are never affected.
 *     - Empty level: setup skips guards and every object except doors, keys
 *       and glass (prop.c proplvreset2), the same way a difficulty-excluded
 *       object is left out.
 *     - Objectives off: the setup's objectives are never registered
 *       (objective.c add_ptr_to_objective), so none show and none can fail.
 *     - Level exits off: an AI list that starts an exit sequence (locks
 *       Bond's controls, ends the level, waits for a button to exit, or
 *       kills Bond in action) is stopped instead (chrai.c).
 *
 *   SPAWN MENU. In any solo mission, B (keyboard) or RB (controller) opens a
 *   menu over the game. Everything placed lands where Bond is looking: a ray
 *   from his view along the floor's stan tiles stops at the first wall or at
 *   the floor it reaches. Characters ask for Enemy or Ally, then a weapon.
 *   Objects reuse the setup's own placement (domakedefaultobj) through a
 *   one-entry pad table, so they sit, scale and collide like level objects.
 *
 * Spawned records live in a pool here; the stage's memory pool owns the
 * models and props, so lvlStageLoad simply forgets them.
 */
#ifndef TARGET_N64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ultra64.h>
#include <bondconstants.h>
#include <bondaicommands.h>

#include "sandbox.h"
#include "game/bondview.h"
#include "game/chr.h"
#include "game/chrai.h"
#include "game/chraction.h"
#include "game/chrobjdata.h"
#include "game/loadobjectmodel.h"
#include "game/matrixmath.h"
#include "game/model.h"
#include "game/objecthandler.h"
#include "game/player.h"
#include "game/propobj.h"
#include "game/stan.h"
#include "game/textrelated.h"
#include "game/dyn.h"
#include "fr.h"
#include "../generated/sandbox_catalog.h"

extern float atan2f(float, float);
extern void portConLogf(const char *fmt, ...);
extern s32 portStageRunning(void);              /* front.c */
extern s32 portP1ControlsLive(void);            /* bondview2.c */
extern void portConsoleSpace(s32 *w, s32 *h);   /* video.c; weak 640x480 headless */
extern void domakedefaultobj(s32 stage, ObjectRecord *obj, s32 cmdindex);              /* prop.c */
extern PropRecord *chrSpawnAtCoord(s32 bodynum, s32 headnum, coord3d *pos, StandTile *stan, f32 angle,
                                   AIListRecord *ailist, s32 spawnflags);               /* chraction.c */
extern bool actor_aim_at_actor(ChrRecord *self, s32 attack_type, s32 target);          /* chraction.c */
extern bool chrGoToBond(ChrRecord *self, SPEED speed);                                 /* chraction.c */
extern u32 weaponLoadProjectileModels(ITEM_IDS item);                                  /* bondview_r.c */
extern s32 bossGetStageNum(void);

/* ==== settings ============================================================ */

s32 g_SandboxEmptyLevel;
s32 g_SandboxNoObjectives;
s32 g_SandboxNoExits;

static s32 sOpen;           /* the spawn menu is up (the menu section below drives it) */
static s32 sArmed;          /* a mission was started from the difficulty screen */
static s32 sEmpty;          /* this mission's latched settings */
static s32 sNoObjectives;
static s32 sNoExits;

void sandboxArmForNextMission(void)
{
    sArmed = 1;
}

s32 sandboxEmptyActive(void)        { return sEmpty; }
s32 sandboxNoObjectivesActive(void) { return sNoObjectives; }
s32 sandboxNoExitsActive(void)      { return sNoExits; }

/* Empty level: guards, pickups, security and vehicles go; the furniture,
 * doors, keys and glass that make up the level stay (some stages build
 * walls and roadblocks out of plain props). */
s32 sandboxSkipPropdef(s32 type)
{
    if (!sEmpty)
    {
        return 0;
    }
    switch (type)
    {
    case PROPDEF_GUARD:
    case PROPDEF_COLLECTABLE:
    case PROPDEF_HAT:
    case PROPDEF_AMMO:
    case PROPDEF_MAGAZINE:
    case PROPDEF_ARMOUR:
    case PROPDEF_CCTV:
    case PROPDEF_AUTOGUN:
    case PROPDEF_ALARM:
    case PROPDEF_TANK:
    case PROPDEF_VEHICHLE:
    case PROPDEF_AIRCRAFT:
        return 1;
    }
    return 0;
}

/* ==== spawn records ======================================================= */

#define SB_MAX_OBJS   192
#define SB_MAX_ALLIES 32
#define SB_MAX_UNDO   256

/* big enough for every record type spawned here (the ammo crate is 0x2d words) */
typedef union {
    ObjectRecord obj;
    WeaponObjRecord weapon;
    MultiAmmoCrateRecord ammo;
    BodyArmourRecord armour;
    u32 words[0x30];
} SbObjSlot;

static SbObjSlot sObjs[SB_MAX_OBJS];
static s32 sObjCount;

/* allies: the chr and its prop, so a reused chr slot is noticed */
static struct { ChrRecord *chr; PropRecord *prop; } sAllies[SB_MAX_ALLIES];
static s32 sAllyCount;

/* undo stack: what was spawned, newest last */
static struct { u8 isChr; ChrRecord *chr; PropRecord *prop; ObjectRecord *obj; } sUndo[SB_MAX_UNDO];
static s32 sUndoCount;

/* the ally AI list: idle forever - sandboxTick drives allies from C, so the
 * standard guard script (which hunts Bond) never runs for them */
static u8 sAllyAiList[] = {
    label(0x00)
    ai_sleep
    goto_first(0x00)
    ai_list_end
};

void sandboxStageLoad(s32 stage)
{
    static s32 sOptsRead;

    if (!sOptsRead)
    {   /* GE_SANDBOX_OPTS: see sbHarnessTick */
        const char *e = getenv("GE_SANDBOX_OPTS");
        sOptsRead = 1;
        if (e != NULL && e[0] != '\0')
        {
            g_SandboxEmptyLevel = strstr(e, "empty") != NULL;
            g_SandboxNoObjectives = strstr(e, "noobj") != NULL;
            g_SandboxNoExits = strstr(e, "noexit") != NULL;
            sArmed = 1;
        }
    }
    sEmpty = sNoObjectives = sNoExits = 0;
    if (stage < 0 || stage >= LEVELID_MAX)
    {
        return; /* the title / frontend stage: the armed settings wait for the mission */
    }
    if (sArmed && getPlayerCount() == 1)
    {
        sEmpty = g_SandboxEmptyLevel;
        sNoObjectives = g_SandboxNoObjectives;
        sNoExits = g_SandboxNoExits;
        if (sEmpty || sNoObjectives || sNoExits)
        {
            portConLogf("ge: sandbox: mission settings - empty level %s, objectives %s, level exits %s\n",
                        sEmpty ? "ON" : "off", sNoObjectives ? "OFF" : "on", sNoExits ? "OFF" : "on");
        }
    }
    sArmed = 0;
    sOpen = 0;
    sObjCount = 0;
    sAllyCount = 0;
    sUndoCount = 0;
    memset(sObjs, 0, sizeof(sObjs));
}

static void sbUndoPush(s32 isChr, ChrRecord *chr, PropRecord *prop, ObjectRecord *obj)
{
    if (sUndoCount >= SB_MAX_UNDO)
    {
        memmove(&sUndo[0], &sUndo[1], sizeof(sUndo[0]) * (SB_MAX_UNDO - 1));
        sUndoCount--;
    }
    sUndo[sUndoCount].isChr = (u8) isChr;
    sUndo[sUndoCount].chr = chr;
    sUndo[sUndoCount].prop = prop;
    sUndo[sUndoCount].obj = obj;
    sUndoCount++;
}

/* ==== level exits ========================================================= */

s32 sandboxBlockLevelExit(void)
{
    if (!sNoExits)
    {
        return 0;
    }
    /* an exit sequence often makes Bond invulnerable and hides his gun just
     * before locking the controls - give both back */
    g_PlayerInvincible = FALSE;
    portConLogf("ge: sandbox: level exit blocked (level exits are off)\n");
    return 1;
}

/* ==== where Bond is looking =============================================== */

/* Walk the view ray along the stan tiles: stop at the first wall (the walk
 * fails) or where the ray drops below the floor. The result is on the floor,
 * pulled back from any wall so a model does not clip into it. */
static s32 sbFindPlacement(coord3d *pos, StandTile **stan, f32 *facing)
{
    PropRecord *bp = getCurrentPlayerProp();
    f32 th, va, dx, dz, cv, sv;
    f32 lastx, lastz, d, dist;
    StandTile *lasttile;
    s32 hitwall = 0;

    if (bp == NULL || bp->stan == NULL || g_CurrentPlayer == NULL)
    {
        return 0;
    }
    th = g_CurrentPlayer->vv_theta * (M_PI_F / 180.0f);
    va = g_CurrentPlayer->vv_verta * (M_PI_F / 180.0f);
    dx = -sinf(th);
    dz = cosf(th);
    cv = cosf(va);
    sv = sinf(va);
    if (cv < 0.05f)
    {
        cv = 0.05f; /* straight down still steps forward a little */
    }

    lastx = bp->pos.x;
    lastz = bp->pos.z;
    lasttile = bp->stan;
    for (d = 20.0f; d <= 3000.0f; d += 20.0f)
    {
        f32 nx = bp->pos.x + dx * cv * d;
        f32 nz = bp->pos.z + dz * cv * d;
        f32 ny = bp->pos.y + sv * d;
        StandTile *t = lasttile;

        if (!walkTilesBetweenPoints_NoCallback(&t, lastx, lastz, nx, nz) || t == NULL)
        {
            hitwall = 1;
            break;
        }
        lastx = nx;
        lastz = nz;
        lasttile = t;
        if (ny <= stanGetPositionYValue(t, nx, nz))
        {
            break; /* the ray reached the floor */
        }
    }

    dist = sqrtf((lastx - bp->pos.x) * (lastx - bp->pos.x) + (lastz - bp->pos.z) * (lastz - bp->pos.z));
    if (hitwall || dist > 2980.0f)
    {
        /* back off 40 units toward Bond, re-walking from him so the tile is right */
        f32 want = dist - 40.0f;
        if (want < 60.0f)
        {
            want = 60.0f;
        }
        if (want < dist)
        {
            StandTile *t = bp->stan;
            f32 nx = bp->pos.x + dx * want;
            f32 nz = bp->pos.z + dz * want;
            if (walkTilesBetweenPoints_NoCallback(&t, bp->pos.x, bp->pos.z, nx, nz) && t != NULL)
            {
                lastx = nx;
                lastz = nz;
                lasttile = t;
            }
        }
    }
    else if (dist < 60.0f)
    {
        /* looking at his own feet: put it just in front of him */
        StandTile *t = bp->stan;
        f32 nx = bp->pos.x + dx * 80.0f;
        f32 nz = bp->pos.z + dz * 80.0f;
        if (walkTilesBetweenPoints_NoCallback(&t, bp->pos.x, bp->pos.z, nx, nz) && t != NULL)
        {
            lastx = nx;
            lastz = nz;
            lasttile = t;
        }
    }

    pos->x = lastx;
    pos->z = lastz;
    pos->y = stanGetPositionYValue(lasttile, lastx, lastz);
    *stan = lasttile;
    *facing = atan2f(-dx, -dz); /* look vector pointing back at Bond */
    return 1;
}

/* ==== objects ============================================================= */

static SbObjSlot *sbAllocObj(void)
{
    SbObjSlot *s;

    if (sObjCount >= SB_MAX_OBJS)
    {
        return NULL;
    }
    s = &sObjs[sObjCount++];
    memset(s, 0, sizeof(*s));
    return s;
}

/* Place a filled-in record with the setup's own routine: a one-entry pad
 * table (or bound-pad table, for glass) stands in for the stage's for the
 * length of the call. */
static s32 sbPlaceRecord(ObjectRecord *rec, coord3d *pos, StandTile *stan, f32 facing,
                         s32 bound, f32 width, f32 height)
{
    PadRecord pad;
    BoundPadRecord bpad;
    PadRecord *savedPads = g_CurrentSetup.pads;
    BoundPadRecord *savedBound = g_CurrentSetup.boundpads;
    f32 lx = sinf(facing), lz = cosf(facing);

    if (rec->obj < 0 || rec->obj >= PROP_MAX || PitemZ_entries[rec->obj].header == NULL)
    {
        return 0;
    }

    if (!bound)
    {
        memset(&pad, 0, sizeof(pad));
        pad.pos = *pos;
        pad.up.y = 1.0f;
        pad.look.x = lx;
        pad.look.z = lz;
        pad.plink = "sandbox";
        pad.stan = stan;
        rec->pad = 0;
        g_CurrentSetup.pads = &pad;
    }
    else
    {
        /* as the stages lay out glass: 'up' is the pane's normal, 'look'
         * is world up, and the box is width x ~0 x height around the centre */
        memset(&bpad, 0, sizeof(bpad));
        bpad.pos.x = pos->x;
        bpad.pos.y = pos->y + height * 0.5f;
        bpad.pos.z = pos->z;
        bpad.up.x = lx;
        bpad.up.z = lz;
        bpad.look.y = 1.0f;
        bpad.plink = "sandbox";
        bpad.stan = stan;
        bpad.bbox.xmin = -width * 0.5f;
        bpad.bbox.xmax = width * 0.5f;
        bpad.bbox.ymin = -0.00005f;
        bpad.bbox.ymax = 0.00005f;
        bpad.bbox.zmin = -height * 0.5f;
        bpad.bbox.zmax = height * 0.5f;
        rec->pad = 10000;
        g_CurrentSetup.boundpads = &bpad;
    }

    /* domakedefaultobj reads damage as the setup's 16.16 word */
    *(s32 *) &rec->damage = 1000 << 16;
    domakedefaultobj(bossGetStageNum(), rec, 0);

    g_CurrentSetup.pads = savedPads;
    g_CurrentSetup.boundpads = savedBound;

    if (rec->prop == NULL || rec->model == NULL)
    {
        return 0;
    }
    sbUndoPush(0, NULL, rec->prop, rec);
    return 1;
}

static void sbInitHeader(ObjectRecord *rec, s32 type, s32 model, u32 flags)
{
    rec->extrascale = 0x100;
    rec->state = 0;
    rec->type = (u8) type;
    rec->obj = (s16) model;
    rec->flags = flags;
    rec->flags2 = 0;
}

static s32 sbSpawnProp(s32 model)
{
    coord3d pos;
    StandTile *stan;
    f32 facing;
    SbObjSlot *s;

    if (!sbFindPlacement(&pos, &stan, &facing) || (s = sbAllocObj()) == NULL)
    {
        return 0;
    }
    sbInitHeader(&s->obj, PROPDEF_PROP, model, PROPFLAG_RENDERPOSTBG);
    return sbPlaceRecord(&s->obj, &pos, stan, facing, 0, 0.0f, 0.0f);
}

static s32 sbSpawnWeapon(s32 item, s32 model)
{
    coord3d pos;
    StandTile *stan;
    f32 facing;
    SbObjSlot *s;

    if (!sbFindPlacement(&pos, &stan, &facing) || (s = sbAllocObj()) == NULL)
    {
        return 0;
    }
    sbInitHeader(&s->obj, PROPDEF_COLLECTABLE, model, PROPFLAG_RENDERPOSTBG);
    s->weapon.weaponnum = (s8) item;
    s->weapon.LinkedWeaponType = -1;
    s->weapon.timer = -1;
    s->weapon.dualweapon = NULL;
    weaponLoadProjectileModels((ITEM_IDS) item);
    return sbPlaceRecord(&s->obj, &pos, stan, facing, 0, 0.0f, 0.0f);
}

static s32 sbSpawnAmmo(s32 ammotype, s32 quantity)
{
    coord3d pos;
    StandTile *stan;
    f32 facing;
    SbObjSlot *s;
    s32 i;

    if (!sbFindPlacement(&pos, &stan, &facing) || (s = sbAllocObj()) == NULL)
    {
        return 0;
    }
    sbInitHeader(&s->obj, PROPDEF_AMMO, PROP_AMMO_CRATE3, PROPFLAG_RENDERPOSTBG);
    s->obj.extrascale = 153; /* the stages' ammo boxes are 0.6 scale */
    for (i = 0; i < AMMOTYPE_GLOBAL_MAX; i++)
    {
        s->ammo.slots[i].modelnum = 0xFFFF;
        s->ammo.slots[i].quantity = 0;
    }
    /* slot n holds ammo type n + 1 (propPickupByPlayer) */
    s->ammo.slots[ammotype - 1].quantity = (u16) quantity;
    return sbPlaceRecord(&s->obj, &pos, stan, facing, 0, 0.0f, 0.0f);
}

static s32 sbSpawnArmour(s32 model, f32 amount)
{
    coord3d pos;
    StandTile *stan;
    f32 facing;
    SbObjSlot *s;

    if (!sbFindPlacement(&pos, &stan, &facing) || (s = sbAllocObj()) == NULL)
    {
        return 0;
    }
    sbInitHeader(&s->obj, PROPDEF_ARMOUR, model, PROPFLAG_RENDERPOSTBG);
    s->obj.extrascale = 384; /* as the stages place it */
    s->armour.initialamount = amount;
    s->armour.amount = amount;
    return sbPlaceRecord(&s->obj, &pos, stan, facing, 0, 0.0f, 0.0f);
}

static s32 sbSpawnGlass(s32 model, f32 width, f32 height)
{
    coord3d pos;
    StandTile *stan;
    f32 facing;
    SbObjSlot *s;

    if (!sbFindPlacement(&pos, &stan, &facing) || (s = sbAllocObj()) == NULL)
    {
        return 0;
    }
    /* the stages' free-standing glass: fit X and Y to the bound pad, flag 2
     * stands the pane up, 0x800 free-standing glass, 0x100 collisions,
     * 0x200 glass env mapping, AI sees through */
    sbInitHeader(&s->obj, PROPDEF_GLASS, model, 0x04000b62);
    return sbPlaceRecord(&s->obj, &pos, stan, facing, 1, width, height);
}

/* ==== characters ========================================================== */

static s32 sbIsAlly(ChrRecord *chr)
{
    s32 i;
    for (i = 0; i < sAllyCount; i++)
    {
        if (sAllies[i].chr == chr && sAllies[i].prop == chr->prop)
        {
            return 1;
        }
    }
    return 0;
}

static s32 sbSpawnChr(s32 body, s32 ally, s32 item, s32 model)
{
    coord3d pos;
    StandTile *stan;
    f32 facing;
    PropRecord *prop;
    ChrRecord *chr;
    void *ailist = ally ? (void *) sAllyAiList : (void *) ailistFindById(GAILIST_STANDARD_GUARD);

    if (!sbFindPlacement(&pos, &stan, &facing) || ailist == NULL)
    {
        return 0;
    }
    /* spawn flag 0x10: allow on screen (the AI's spawns insist on off-screen) */
    prop = chrSpawnAtCoord(body, -1, &pos, stan, facing, (AIListRecord *) ailist, 0x10);
    if (prop == NULL || prop->chr == NULL)
    {
        return 0;
    }
    chr = prop->chr;
    if (item != ITEM_UNARMED && model >= 0)
    {
        weaponLoadProjectileModels((ITEM_IDS) item);
        chrGiveWeapon(chr, model, (ITEM_IDS) item, 0);
    }
    if (ally && sAllyCount < SB_MAX_ALLIES)
    {
        sAllies[sAllyCount].chr = chr;
        sAllies[sAllyCount].prop = prop;
        sAllyCount++;
    }
    sbUndoPush(1, chr, prop, NULL);
    return 1;
}

/* Allies, twice a second each: shoot the nearest living non-ally in range,
 * otherwise keep up with Bond. The engine's chr-vs-chr fire (ATTACKTYPE_CHR)
 * does the aiming, and a hit damages whoever is in the line of fire. */
static void sbAllyTick(void)
{
    static s32 sTick;
    PropRecord *bp = getCurrentPlayerProp();
    s32 i, j;

    if (++sTick % 30 != 0 || bp == NULL)
    {
        return;
    }
    for (i = 0; i < sAllyCount; i++)
    {
        ChrRecord *ally = sAllies[i].chr;
        ChrRecord *best = NULL;
        f32 bestd = 1800.0f * 1800.0f;
        f32 bx, bz;

        if (ally->prop != sAllies[i].prop || ally->model == NULL || chrIsDead(ally))
        {
            sAllies[i] = sAllies[--sAllyCount]; /* dead or gone */
            i--;
            continue;
        }
        for (j = 0; j < g_NumChrSlots; j++)
        {
            ChrRecord *c = &g_ChrSlots[j];
            f32 ddx, ddy, ddz, d2;

            if (c == ally || c->model == NULL || c->prop == NULL || chrIsDead(c) || sbIsAlly(c)
                || (c->chrflags & CHRFLAG_HIDDEN))
            {
                continue;
            }
            ddx = c->prop->pos.x - ally->prop->pos.x;
            ddy = c->prop->pos.y - ally->prop->pos.y;
            ddz = c->prop->pos.z - ally->prop->pos.z;
            d2 = ddx * ddx + ddy * ddy + ddz * ddz;
            if (d2 < bestd)
            {
                bestd = d2;
                best = c;
            }
        }
        if (best != NULL)
        {
            if (ally->actiontype != ACT_ATTACK)
            {
                actor_aim_at_actor(ally, ATTACKTYPE_CHR, best->chrnum);
            }
            continue;
        }
        bx = bp->pos.x - ally->prop->pos.x;
        bz = bp->pos.z - ally->prop->pos.z;
        if (bx * bx + bz * bz > 300.0f * 300.0f)
        {
            chrGoToBond(ally, SPEED_RUN);
        }
    }
}

/* ==== tools =============================================================== */

static void sbRemoveEntry(s32 i)
{
    if (sUndo[i].isChr)
    {
        ChrRecord *chr = sUndo[i].chr;
        if (chr->prop == sUndo[i].prop && chr->model != NULL)
        {
            chr->hidden |= CHRHIDDEN_REMOVE; /* the chr manager removes it next tick */
        }
    }
    else
    {
        ObjectRecord *obj = sUndo[i].obj;
        PropRecord *prop = sUndo[i].prop;
        /* still ours: not collected, destroyed-and-freed, or reused */
        if (obj->prop == prop && prop != NULL && prop->type == PROP_TYPE_OBJ && prop->obj == obj && obj->model != NULL)
        {
            objFreePermanently(obj, TRUE);
        }
    }
}

static void sbUndoLast(void)
{
    if (sUndoCount > 0)
    {
        sUndoCount--;
        sbRemoveEntry(sUndoCount);
    }
}

static void sbClearAll(void)
{
    while (sUndoCount > 0)
    {
        sbUndoLast();
    }
    sAllyCount = 0;
}

/* ==== catalogs ============================================================ */

static const struct { const char *name; s8 item; s16 model; s8 guardok; } sWeapons[] = {
    { "Unarmed",             ITEM_UNARMED,       -1,                    1 },
    { "Hunting Knife",       ITEM_KNIFE,         PROP_CHRKNIFE,         1 },
    { "Throwing Knife",      ITEM_THROWKNIFE,    PROP_CHRTHROWKNIFE,    0 },
    { "PP7",                 ITEM_WPPK,          PROP_CHRWPPK,          1 },
    { "PP7 (Silenced)",      ITEM_WPPKSIL,       PROP_CHRWPPKSIL,       1 },
    { "DD44 Dostovei",       ITEM_TT33,          PROP_CHRTT33,          1 },
    { "Klobb",               ITEM_SKORPION,      PROP_CHRSKORPION,      1 },
    { "KF7 Soviet",          ITEM_AK47,          PROP_CHRKALASH,        1 },
    { "ZMG (9mm)",           ITEM_UZI,           PROP_CHRUZI,           1 },
    { "D5K Deutsche",        ITEM_MP5K,          PROP_CHRMP5K,          1 },
    { "D5K (Silenced)",      ITEM_MP5KSIL,       PROP_CHRMP5KSIL,       1 },
    { "Phantom",             ITEM_SPECTRE,       PROP_CHRSPECTRE,       1 },
    { "AR33 Assault Rifle",  ITEM_M16,           PROP_CHRM16,           1 },
    { "RC-P90",              ITEM_FNP90,         PROP_CHRFNP90,         1 },
    { "Shotgun",             ITEM_SHOTGUN,       PROP_CHRSHOTGUN,       1 },
    { "Automatic Shotgun",   ITEM_AUTOSHOT,      PROP_CHRAUTOSHOT,      1 },
    { "Sniper Rifle",        ITEM_SNIPERRIFLE,   PROP_CHRSNIPERRIFLE,   1 },
    { "Cougar Magnum",       ITEM_RUGER,         PROP_CHRRUGER,         1 },
    { "Golden Gun",          ITEM_GOLDENGUN,     PROP_CHRGOLDEN,        1 },
    { "Silver PP7",          ITEM_SILVERWPPK,    PROP_CHRSILVERWPPK,    1 },
    { "Gold PP7",            ITEM_GOLDWPPK,      PROP_CHRGOLDWPPK,      1 },
    { "Moonraker Laser",     ITEM_LASER,         PROP_CHRLASER,         1 },
    { "Grenade Launcher",    ITEM_GRENADELAUNCH, PROP_CHRGRENADELAUNCH, 1 },
    { "Rocket Launcher",     ITEM_ROCKETLAUNCH,  PROP_CHRROCKETLAUNCH,  1 },
    { "Hand Grenade",        ITEM_GRENADE,       PROP_CHRGRENADE,       0 },
    { "Timed Mine",          ITEM_TIMEDMINE,     PROP_CHRTIMEDMINE,     0 },
    { "Proximity Mine",      ITEM_PROXIMITYMINE, PROP_CHRPROXIMITYMINE, 0 },
    { "Remote Mine",         ITEM_REMOTEMINE,    PROP_CHRREMOTEMINE,    0 },
};
#define SB_NUM_WEAPONS ((s32) (sizeof(sWeapons) / sizeof(sWeapons[0])))

static const struct { const char *name; s8 type; s16 quantity; } sAmmo[] = {
    { "9mm Rounds (100)",          AMMO_9MM,         100 },
    { "Rifle Rounds (100)",        AMMO_RIFLE,       100 },
    { "Shotgun Cartridges (40)",   AMMO_SHOTGUN,     40 },
    { "Magnum Rounds (30)",        AMMO_MAGNUM,      30 },
    { "Golden Bullets (10)",       AMMO_GGUN,        10 },
    { "Hand Grenades (6)",         AMMO_GRENADE,     6 },
    { "Grenade Rounds (12)",       AMMO_GRENADEROUND, 12 },
    { "Rockets (6)",               AMMO_ROCKETS,     6 },
    { "Throwing Knives (10)",      AMMO_KNIFE,       10 },
    { "Remote Mines (6)",          AMMO_REMOTEMINE,  6 },
    { "Proximity Mines (6)",       AMMO_PROXMINE,    6 },
    { "Timed Mines (6)",           AMMO_TIMEDMINE,   6 },
};
#define SB_NUM_AMMO ((s32) (sizeof(sAmmo) / sizeof(sAmmo[0])))

static const struct { const char *name; s16 model; f32 amount; } sArmour[] = {
    { "Body Armor (Full)",  PROP_BODYARMOUR,     1.0f },
    { "Body Armor (Half)",  PROP_BODYARMOURVEST, 0.5f },
};
#define SB_NUM_ARMOUR ((s32) (sizeof(sArmour) / sizeof(sArmour[0])))

static const struct { const char *name; s16 model; s16 w; s16 h; } sGlass[] = {
    { "Window Glass - Small",           PROP_WINDOW,         100, 100 },
    { "Window Glass - Medium",          PROP_WINDOW,         180, 190 },
    { "Window Glass - Large Wall",      PROP_WINDOW,         400, 250 },
    { "Lattice Window (4x10)",          PROP_WINDOW_LIB_LG1, 180, 190 },
    { "Lattice Window (4x3)",           PROP_WINDOW_LIB_SM1, 180, 190 },
    { "Lattice Window (4x4)",           PROP_WINDOW_COR11,   180, 190 },
};
#define SB_NUM_GLASS ((s32) (sizeof(sGlass) / sizeof(sGlass[0])))

#define SB_NUM_PROPS  ((s32) (sizeof(sSandboxProps) / sizeof(sSandboxProps[0])))
#define SB_NUM_BODIES ((s32) (sizeof(sSandboxBodies) / sizeof(sSandboxBodies[0])))

/* ==== menu ================================================================ */

enum {
    PAGE_MAIN,
    PAGE_CHR_BODY,
    PAGE_CHR_TEAM,
    PAGE_CHR_WEAPON,
    PAGE_WEAPON,
    PAGE_AMMO,
    PAGE_ARMOUR,
    PAGE_GLASS,
    PAGE_PROPS,
    PAGE_TOOLS
};

static const char *const sMainItems[] = {
    "Characters / Guards",
    "Weapons",
    "Ammo",
    "Body Armor",
    "Glass Windows",
    "Objects (all props)",
    "Tools",
    "Close",
};

static s32 sOpen;
static struct { s32 page; s32 sel; s32 top; } sStack[6];
static s32 sDepth;
static s32 sPickBody, sPickAlly;
static char sStatus[96];
static OSTime sStatusUntil;

/* key events from input_pc.c, drained by sandboxTick */
static volatile s32 sKeyQueue[16];
static volatile s32 sKeyHead, sKeyTail;

/* map the guard-usable weapons for the character weapon page */
static s32 sGuardWeaponMap[SB_NUM_WEAPONS];
static s32 sGuardWeaponCount;

static void sbStatus(const char *fmt, const char *arg)
{
    snprintf(sStatus, sizeof(sStatus), fmt, arg);
    sStatusUntil = osGetTime() + 2500ull * 46875ull; /* 2.5 s of osGetTime() */
}

static s32 sbPageCount(s32 page)
{
    switch (page)
    {
    case PAGE_MAIN:       return (s32) (sizeof(sMainItems) / sizeof(sMainItems[0]));
    case PAGE_CHR_BODY:   return SB_NUM_BODIES;
    case PAGE_CHR_TEAM:   return 2;
    case PAGE_CHR_WEAPON: return sGuardWeaponCount;
    case PAGE_WEAPON:     return SB_NUM_WEAPONS - 1; /* no "Unarmed" pickup */
    case PAGE_AMMO:       return SB_NUM_AMMO;
    case PAGE_ARMOUR:     return SB_NUM_ARMOUR;
    case PAGE_GLASS:      return SB_NUM_GLASS;
    case PAGE_PROPS:      return SB_NUM_PROPS;
    case PAGE_TOOLS:      return 3;
    }
    return 0;
}

static const char *sbPageTitle(s32 page)
{
    switch (page)
    {
    case PAGE_MAIN:       return "SANDBOX - SPAWN MENU";
    case PAGE_CHR_BODY:   return "CHARACTERS - pick a body";
    case PAGE_CHR_TEAM:   return "CHARACTERS - enemy or ally?";
    case PAGE_CHR_WEAPON: return "CHARACTERS - pick a weapon";
    case PAGE_WEAPON:     return "WEAPONS";
    case PAGE_AMMO:       return "AMMO";
    case PAGE_ARMOUR:     return "BODY ARMOR";
    case PAGE_GLASS:      return "GLASS WINDOWS";
    case PAGE_PROPS:      return "OBJECTS";
    case PAGE_TOOLS:      return "TOOLS";
    }
    return "";
}

static void sbItemLabel(s32 page, s32 i, char *buf, s32 n)
{
    switch (page)
    {
    case PAGE_MAIN:
        snprintf(buf, n, "%s", sMainItems[i]);
        break;
    case PAGE_CHR_BODY:
        snprintf(buf, n, "%s", sSandboxBodies[i].name);
        break;
    case PAGE_CHR_TEAM:
        snprintf(buf, n, "%s", i == 0 ? "Enemy (fights Bond)" : "Ally (follows Bond, fights enemies)");
        break;
    case PAGE_CHR_WEAPON:
        snprintf(buf, n, "%s", sWeapons[sGuardWeaponMap[i]].name);
        break;
    case PAGE_WEAPON:
        snprintf(buf, n, "%s", sWeapons[i + 1].name);
        break;
    case PAGE_AMMO:
        snprintf(buf, n, "%s", sAmmo[i].name);
        break;
    case PAGE_ARMOUR:
        snprintf(buf, n, "%s", sArmour[i].name);
        break;
    case PAGE_GLASS:
        snprintf(buf, n, "%s", sGlass[i].name);
        break;
    case PAGE_PROPS:
        snprintf(buf, n, "%3d  %s", (int) sSandboxProps[i].model, sSandboxProps[i].name);
        break;
    case PAGE_TOOLS:
        if (i == 0)
        {
            snprintf(buf, n, "Undo last spawn (%d placed)", (int) sUndoCount);
        }
        else if (i == 1)
        {
            snprintf(buf, n, "Remove everything I spawned");
        }
        else
        {
            snprintf(buf, n, "Invincibility: %s", g_PlayerInvincible ? "ON" : "off");
        }
        break;
    default:
        buf[0] = '\0';
        break;
    }
}

static void sbPush(s32 page)
{
    if (sDepth < (s32) (sizeof(sStack) / sizeof(sStack[0])) - 1)
    {
        sDepth++;
        sStack[sDepth].page = page;
        sStack[sDepth].sel = 0;
        sStack[sDepth].top = 0;
    }
}

static void sbOpen(void)
{
    s32 i;

    sOpen = 1;
    sDepth = 0;
    sStack[0].page = PAGE_MAIN;
    sStack[0].sel = 0;
    sStack[0].top = 0;
    sGuardWeaponCount = 0;
    for (i = 0; i < SB_NUM_WEAPONS; i++)
    {
        if (sWeapons[i].guardok)
        {
            sGuardWeaponMap[sGuardWeaponCount++] = i;
        }
    }
}

static void sbClose(void)
{
    sOpen = 0;
}

static void sbSpawned(s32 ok, const char *what)
{
    if (ok)
    {
        sbStatus("Placed: %s", what);
    }
    else
    {
        sbStatus("Could not place %s here (no floor, or out of slots)", what);
    }
}

static void sbAccept(void)
{
    s32 page = sStack[sDepth].page;
    s32 sel = sStack[sDepth].sel;
    char label[96];

    if (sbPageCount(page) == 0)
    {
        return;
    }
    sbItemLabel(page, sel, label, sizeof(label));

    switch (page)
    {
    case PAGE_MAIN:
        switch (sel)
        {
        case 0: sbPush(PAGE_CHR_BODY); break;
        case 1: sbPush(PAGE_WEAPON);   break;
        case 2: sbPush(PAGE_AMMO);     break;
        case 3: sbPush(PAGE_ARMOUR);   break;
        case 4: sbPush(PAGE_GLASS);    break;
        case 5: sbPush(PAGE_PROPS);    break;
        case 6: sbPush(PAGE_TOOLS);    break;
        default: sbClose();            break;
        }
        break;
    case PAGE_CHR_BODY:
        sPickBody = sSandboxBodies[sel].body;
        sbPush(PAGE_CHR_TEAM);
        break;
    case PAGE_CHR_TEAM:
        sPickAlly = sel;
        sbPush(PAGE_CHR_WEAPON);
        break;
    case PAGE_CHR_WEAPON:
    {
        s32 w = sGuardWeaponMap[sel];
        sbSpawned(sbSpawnChr(sPickBody, sPickAlly, sWeapons[w].item, sWeapons[w].model),
                  sPickAlly ? "ally" : "enemy");
        /* stay here: placing several of the same is the common case */
        break;
    }
    case PAGE_WEAPON:
        sbSpawned(sbSpawnWeapon(sWeapons[sel + 1].item, sWeapons[sel + 1].model), label);
        break;
    case PAGE_AMMO:
        sbSpawned(sbSpawnAmmo(sAmmo[sel].type, sAmmo[sel].quantity), label);
        break;
    case PAGE_ARMOUR:
        sbSpawned(sbSpawnArmour(sArmour[sel].model, sArmour[sel].amount), label);
        break;
    case PAGE_GLASS:
        sbSpawned(sbSpawnGlass(sGlass[sel].model, sGlass[sel].w, sGlass[sel].h), label);
        break;
    case PAGE_PROPS:
        sbSpawned(sbSpawnProp(sSandboxProps[sel].model), sSandboxProps[sel].name);
        break;
    case PAGE_TOOLS:
        if (sel == 0)
        {
            sbUndoLast();
            sbStatus("%s", "Removed the last thing placed");
        }
        else if (sel == 1)
        {
            sbClearAll();
            sbStatus("%s", "Removed everything placed");
        }
        else
        {
            g_PlayerInvincible = !g_PlayerInvincible;
        }
        break;
    }
}

static void sbBack(void)
{
    if (sDepth > 0)
    {
        sDepth--;
    }
    else
    {
        sbClose();
    }
}

#define SB_VISIBLE_ROWS 14

static void sbMove(s32 delta)
{
    s32 n = sbPageCount(sStack[sDepth].page);
    s32 sel;

    if (n <= 0)
    {
        return;
    }
    sel = sStack[sDepth].sel + delta;
    if (delta == 1 || delta == -1)
    {
        sel = (sel + n) % n; /* single steps wrap */
    }
    else
    {
        if (sel < 0) sel = 0;
        if (sel >= n) sel = n - 1;
    }
    sStack[sDepth].sel = sel;
    if (sel < sStack[sDepth].top)
    {
        sStack[sDepth].top = sel;
    }
    if (sel >= sStack[sDepth].top + SB_VISIBLE_ROWS)
    {
        sStack[sDepth].top = sel - SB_VISIBLE_ROWS + 1;
    }
}

static s32 sbAvailable(void)
{
    return portStageRunning() && getPlayerCount() == 1 && portP1ControlsLive();
}

s32 sandboxKeyDown(s32 key)
{
    s32 next;

    if (!sOpen && !(key == SANDBOX_KEY_TOGGLE && sbAvailable()))
    {
        return 0;
    }
    next = (sKeyHead + 1) % 16;
    if (next != sKeyTail)
    {
        sKeyQueue[sKeyHead] = key;
        sKeyHead = next;
    }
    return 1;
}

static void sbHandle(s32 key)
{
    switch (key)
    {
    case SANDBOX_KEY_TOGGLE:
        if (sOpen) sbClose(); else sbOpen();
        break;
    case SANDBOX_KEY_UP:       if (sOpen) sbMove(-1); break;
    case SANDBOX_KEY_DOWN:     if (sOpen) sbMove(1); break;
    case SANDBOX_KEY_PAGEUP:   if (sOpen) sbMove(-SB_VISIBLE_ROWS); break;
    case SANDBOX_KEY_PAGEDOWN: if (sOpen) sbMove(SB_VISIBLE_ROWS); break;
    case SANDBOX_KEY_RIGHT:
    case SANDBOX_KEY_ACCEPT:   if (sOpen) sbAccept(); break;
    case SANDBOX_KEY_LEFT:
    case SANDBOX_KEY_BACK:     if (sOpen) sbBack(); break;
    }
}

s32 portSandboxCapturing(void)
{
    /* RB held counts too, so the press that opens the menu never reaches
     * the game as its "next weapon" bind */
    return sOpen || (sbAvailable() && (portSandboxPadButtons() & SBPAD_RB));
}

/* controller: edges, plus hold-to-repeat on the d-pad / stick and triggers */
static void sbPadTick(void)
{
    static u32 sPrev;
    static OSTime sRepeatAt;
    const OSTime now = osGetTime();
    const u32 cur = portSandboxPadButtons();
    const u32 pressed = cur & ~sPrev;
    const u32 rep = SBPAD_UP | SBPAD_DOWN | SBPAD_LT | SBPAD_RT;

    if (pressed & SBPAD_RB)
    {
        if (sOpen) sbClose(); else if (sbAvailable()) sbOpen();
    }
    if (sOpen)
    {
        u32 fire = pressed;

        if ((cur & rep) && !(pressed & rep) && now >= sRepeatAt)
        {
            fire |= cur & rep;
            sRepeatAt = now + 70ull * 46875ull;
        }
        else if (pressed & rep)
        {
            sRepeatAt = now + 350ull * 46875ull;
        }
        if (fire & SBPAD_UP)    sbMove(-1);
        if (fire & SBPAD_DOWN)  sbMove(1);
        if (fire & SBPAD_LT)    sbMove(-SB_VISIBLE_ROWS);
        if (fire & SBPAD_RT)    sbMove(SB_VISIBLE_ROWS);
        if (pressed & (SBPAD_A | SBPAD_RIGHT)) sbAccept();
        if (pressed & (SBPAD_B | SBPAD_LEFT))  sbBack();
    }
    sPrev = cur;
}

/* GE_SANDBOX_TEST="cmd;cmd;..." (test harness, like GE_STAGE/GE_SHOT): once
 * Bond has control, run one command every 20 ticks so placements can be
 * checked in GE_SHOT captures without a keyboard.
 *   open | close | down | accept | back     drive the menu
 *   turn:<degrees>                         turn Bond (spreads placements out)
 *   look:<degrees>                         set the vertical look angle
 *   chr:<body#>:<0 enemy|1 ally>:<weapon#>  sSandboxBodies / sWeapons indices
 *   wep:<#> ammo:<#> armor:<#> glass:<#>    catalog indices
 *   prop:<PROP model number>
 * GE_SANDBOX_OPTS="empty,noobj,noexit" arms those settings for the first
 * mission even when it is launched with GE_STAGE. */
static void sbHarnessTick(void)
{
    static s32 sInit, sWait = 180;
    static const char *sNext;
    char cmd[48];
    s32 len, a = 0, b = 0, c = 0;

    if (!sInit)
    {
        sInit = 1;
        sNext = getenv("GE_SANDBOX_TEST");
    }
    if (sNext == NULL || *sNext == '\0' || !sbAvailable())
    {
        return;
    }
    if (--sWait > 0)
    {
        return;
    }
    sWait = 20;

    for (len = 0; sNext[len] != '\0' && sNext[len] != ';' && len < (s32) sizeof(cmd) - 1; len++)
    {
        cmd[len] = sNext[len];
    }
    cmd[len] = '\0';
    sNext += len;
    if (*sNext == ';')
    {
        sNext++;
    }

    if (strcmp(cmd, "open") == 0)        sbOpen();
    else if (strcmp(cmd, "close") == 0)  sbClose();
    else if (strcmp(cmd, "down") == 0)   sbMove(1);
    else if (strcmp(cmd, "accept") == 0) sbAccept();
    else if (strcmp(cmd, "back") == 0)   sbBack();
    else if (sscanf(cmd, "turn:%d", &a) == 1)
    {
        g_CurrentPlayer->vv_theta += (f32) a;
        while (g_CurrentPlayer->vv_theta >= 360.0f) g_CurrentPlayer->vv_theta -= 360.0f;
        while (g_CurrentPlayer->vv_theta < 0.0f)    g_CurrentPlayer->vv_theta += 360.0f;
    }
    else if (sscanf(cmd, "look:%d", &a) == 1)
    {
        g_CurrentPlayer->vv_verta = (f32) a;
    }
    else if (sscanf(cmd, "chr:%d:%d:%d", &a, &b, &c) == 3 && a >= 0 && a < SB_NUM_BODIES && c >= 0 && c < SB_NUM_WEAPONS)
    {
        sbSpawned(sbSpawnChr(sSandboxBodies[a].body, b, sWeapons[c].item, sWeapons[c].model), b ? "ally" : "enemy");
    }
    else if (sscanf(cmd, "wep:%d", &a) == 1 && a > 0 && a < SB_NUM_WEAPONS)
    {
        sbSpawned(sbSpawnWeapon(sWeapons[a].item, sWeapons[a].model), sWeapons[a].name);
    }
    else if (sscanf(cmd, "ammo:%d", &a) == 1 && a >= 0 && a < SB_NUM_AMMO)
    {
        sbSpawned(sbSpawnAmmo(sAmmo[a].type, sAmmo[a].quantity), sAmmo[a].name);
    }
    else if (sscanf(cmd, "armor:%d", &a) == 1 && a >= 0 && a < SB_NUM_ARMOUR)
    {
        sbSpawned(sbSpawnArmour(sArmour[a].model, sArmour[a].amount), sArmour[a].name);
    }
    else if (sscanf(cmd, "glass:%d", &a) == 1 && a >= 0 && a < SB_NUM_GLASS)
    {
        sbSpawned(sbSpawnGlass(sGlass[a].model, sGlass[a].w, sGlass[a].h), sGlass[a].name);
    }
    else if (sscanf(cmd, "prop:%d", &a) == 1 && a >= 0 && a < PROP_MAX)
    {
        sbSpawned(sbSpawnProp(a), "prop");
    }
    else
    {
        portConLogf("ge: sandbox test: unknown command \"%s\"\n", cmd);
        return;
    }
    portConLogf("ge: sandbox test: %s -> %s\n", cmd, sStatus);
}

void sandboxTick(void)
{
    if (getPlayerCount() != 1)
    {
        sOpen = 0;
        return;
    }
    sbHarnessTick();
    while (sKeyTail != sKeyHead)
    {
        s32 key = sKeyQueue[sKeyTail];
        sKeyTail = (sKeyTail + 1) % 16;
        sbHandle(key);
    }
    sbPadTick();
    if (sOpen && !sbAvailable())
    {
        sbClose(); /* Bond died, the watch opened, a cutscene took over */
    }
    sbAllyTick();
}

/* ==== drawing ============================================================= */

/* the console's own 2D space (port_console.c conSpaceBegin): 480 lines by
 * the window's full width, outside the 4:3 pin */
static s32 sSpaceW = 640, sSpaceH = 480;
static s16 sSavedViX, sSavedViY;
#define SB_SPACE(gdl, w, h) do { (gdl)->words.w0 = (u32) 0x4c << 24 /* G_CONSOLE2D_EXT */; \
                                 (gdl)->words.w1 = ((u32) (w) << 16) | (u32) (h); (gdl)++; } while (0)

#define SB_COL_TITLE  0xFFD040FF
#define SB_COL_TEXT   0xE0E0E0FF
#define SB_COL_SEL    0x60FF60FF
#define SB_COL_HINT   0x9090A0FF
#define SB_COL_STATUS 0xFFFFFFFF
#define SB_ROW_PITCH  14
#define SB_X          16
#define SB_Y          36
#define SB_W          360
#define SB_GFX_ROW(n) (3 + (s32) (n) * 12)

static Gfx *sbRow(Gfx *gdl, s32 x, s32 y, const char *text, u32 colour)
{
    s32 tx = x, ty = y;

    if (text[0] == '\0' || dynGetFreeGfx2(gdl) < SB_GFX_ROW(strlen(text)) + 16)
    {
        return gdl;
    }
    return textRender(gdl, &tx, &ty, (char *) text, ptrFontBankGothicChars, ptrFontBankGothic,
                      colour, sSpaceW, sSpaceH, 0, 0);
}

Gfx *sandboxRender(Gfx *gdl)
{
    s32 page, n, i, rows, y;
    char buf[128];
    const s32 showStatus = sStatus[0] != '\0' && osGetTime() < sStatusUntil;

    if ((!sOpen && !showStatus) || ptrFontBankGothic == NULL || ptrFontBankGothicChars == NULL
        || !portStageRunning() || dynGetFreeGfx2(gdl) < 400)
    {
        return gdl;
    }

    portConsoleSpace(&sSpaceW, &sSpaceH);
    sSavedViX = viGetX();
    sSavedViY = viGetY();
    viSetXY((s16) sSpaceW, (s16) sSpaceH);
    SB_SPACE(gdl, sSpaceW, sSpaceH);
    gdl = microcode_constructor(gdl);
    gDPSetScissor(gdl++, G_SC_NON_INTERLACE, 0, 0, sSpaceW, sSpaceH);

    if (!sOpen)
    {
        /* closed: just the last spawn result, bottom left */
        gdl = microcode_constructor_related_to_menus(gdl, SB_X - 6, sSpaceH - 60, SB_X + SB_W, sSpaceH - 38, 0xA0);
        gdl = sbRow(gdl, SB_X, sSpaceH - 56, sStatus, SB_COL_STATUS);
    }
    else
    {
        page = sStack[sDepth].page;
        n = sbPageCount(page);
        rows = n < SB_VISIBLE_ROWS ? n : SB_VISIBLE_ROWS;

        gdl = microcode_constructor_related_to_menus(gdl, SB_X - 8, SB_Y - 8, SB_X + SB_W,
                                                     SB_Y + SB_ROW_PITCH * (rows + 5) + 4, 0xC8);
        snprintf(buf, sizeof(buf), "%s", sbPageTitle(page));
        gdl = sbRow(gdl, SB_X, SB_Y, buf, SB_COL_TITLE);
        if (n > SB_VISIBLE_ROWS)
        {
            snprintf(buf, sizeof(buf), "%d / %d", (int) (sStack[sDepth].sel + 1), (int) n);
            gdl = sbRow(gdl, SB_X + SB_W - 70, SB_Y, buf, SB_COL_HINT);
        }

        y = SB_Y + SB_ROW_PITCH + 6;
        for (i = 0; i < rows; i++)
        {
            const s32 idx = sStack[sDepth].top + i;
            const s32 sel = idx == sStack[sDepth].sel;
            char label[96];

            sbItemLabel(page, idx, label, sizeof(label));
            snprintf(buf, sizeof(buf), "%s %s", sel ? ">" : " ", label);
            gdl = sbRow(gdl, SB_X, y, buf, sel ? SB_COL_SEL : SB_COL_TEXT);
            y += SB_ROW_PITCH;
        }

        y += 6;
        gdl = sbRow(gdl, SB_X, y, "Up/Down: choose   Enter/pad A: select   Bksp/pad B: back", SB_COL_HINT);
        y += SB_ROW_PITCH;
        gdl = sbRow(gdl, SB_X, y, "PgUp/PgDn or LT/RT: page   B/RB: close", SB_COL_HINT);
        y += SB_ROW_PITCH;
        if (showStatus)
        {
            gdl = sbRow(gdl, SB_X, y, sStatus, SB_COL_STATUS);
        }
    }

    gdl = combiner_bayer_lod_perspective(gdl);
    SB_SPACE(gdl, 0, 0);
    viSetXY(sSavedViX, sSavedViY);
    return gdl;
}

#endif /* TARGET_N64 */
