# Quest VR: Holomatch H1 (any server, engine only)

Status: plan, approved by the owner on 2026-11-26. Round 1 (spikes S1, S2, S4 and steps 1–4, the
aim) is being built; §10 records what it found. See [PLAN.md](PLAN.md) for the phases,
[MOVEMENT.md](MOVEMENT.md) for the head and movement design, [RENDERING.md](RENDERING.md) for the
eye rendering, and [QUEST.md](QUEST.md) for the controller input.

**Goal.** A Quest player plays on **any** Holomatch server, stock 1.2 or third-party. Players who
aren't ours see a normal player. The server never needs to know.

**How to read the references:**
- Engine paths are under `EFAndroid-SP/app/jni/efcode/` on `feature/quest-vr` (79606ff).
- `cmod:<file>:<lines>` means `https://github.com/Chomenor/cmod-game-code/blob/main/code/<file>`,
  read online on 2026-11-26 (last pushed 2026-10-30).
- cMod's code is Raven's Holomatch source plus cMod's changes. Where this says that stock 1.2 does
  the same, that is an inference for the parts cMod changed. The spikes in §9 check it.
- **[guess]** marks an estimate or an inference that no code or test has confirmed yet.

---

## 1. User requirements, and the answers

| # | Requirement | Answer in this plan |
|---|---|---|
| R1 | A VR player joins any normal, non-VR server. The server never needs to know, and nothing on the server side is required. | Everything is in the engine, behind `EF_VR` and active only while VR runs in Holomatch. The usercmd, the protocol, the game modules, the paks and the userinfo all stay as they are (§2). The server receives ordinary view angles, movement and buttons. |
| R2 | Disadvantages for the VR player are fine if they keep compatibility and fairness. Prefer what is honest and visible to others. Never an unseen advantage. | **The head may move the eye only about 4 units (12 cm) from the body's centre, inside the drawn head.** Past that, **the whole body walks after the head**, so leaning around a corner steps the hitbox out where everyone can see it. Where the body can't follow (a wall, a ledge, the air), the view stops at that small radius and fades. The eye is never higher than the game's own eye. §5 re-evaluates the options. |
| R3 | Must not be flagged as cheating. | cMod has no aim, angle-rate or userinfo checks that a VR player could trip (§7). Pitch stays inside the server's clamp, roll is always 0, the usercmd timing is the engine's normal timing, and slow room-scale steps stay within cMod's walk-button rule. **No `vr` userinfo key by default:** stock servers ignore it, but other clients never receive it, so it can't help our clients recognise each other. It would only label the player to server admins. There is no quieter honest channel (§7.4). |
| R4 | Optional: our clients draw VR players more accurately, but never in contradiction with the server. | **Recommended against for H1.** On a stock server our clients can't tell who is in VR, and they have no pose data. Any hand-held gun or gaze-turned head would contradict what the server simulates: shots leave from the table muzzle beside the eye, and the head faces the aim. That would mislead players about where shots come from (§6.3). This belongs to H2. |

---

## 2. What H1 may and may not touch

- **Unchanged:** `usercmd_t` and its wire encoding (`qcommon/q_shared.h:1356-1364`, `qcommon/msg.c:810-829`),
  protocol 24 and 26, the cMod and stock modules, the paks.
- **Ours:** the engine's input (`client/cl_input.c`), the cgame syscall layer (`client/cl_cgame.c`),
  the renderer (`renderervk/tr_vr.c`) and the VR module (`vr/`).
- **Only one channel to the server:** the usercmd's angles, buttons, weapon and movement
  (cmod:common/q_shared.h:1052-1058). The server learns nothing about the head or the hands except
  through them.
- **Gate:** active only when `vr_active 1`, `com_holomatch 1`, and the cgame runs through
  `CL_CgameSystemCalls` (`client/cl_cgame.c:626`), not the single-player bridge. Call it
  `VR_HM_Active()`. Flat builds compile none of it. Desktop `EF_VR` builds without a headset never
  set `vr_active`.

---

## 3. Aim: from the hand to the view angles

### 3.1 How the server fires (cMod, Raven's code)

1. `FireWeapon` takes `AngleVectors( ps.viewangles )` (cmod:game/g_weapon.c:2455). Roll is part of
   those angles, so it tilts `right` and `up`.
2. `CalcMuzzlePoint` (cmod:game/g_weapon.c:2250-2277) starts at `s.pos.trBase`. It adds the
   per-weapon `WP_MuzzlePoint[]` offset along forward and right, plus `viewheight` and the table's
   up value on world Z (table at :2189-2204; phaser 29/2/−4, compression rifle 25/7/−10).
3. `CorrectForwardVector` (cmod:game/g_weapon.c:2143-2183):
   - **(a)** It traces from the eye (`ps.origin + viewheight`) to the muzzle. If that is blocked, the
     muzzle becomes the eye.
   - **(b)** Otherwise it traces from the eye along the view direction, up to 8192 units, and turns
     the shot so it flies from the muzzle **to that point**.

**So the impact is wherever the server's eye ray, along the sent angles, first hits.** The only
exception is something between the table muzzle and that point.

### 3.2 Choosing the angles

Each client frame, in the engine (new `vr/vr_hm_aim.c`, called from `CL_CreateCmd` after
`VR_Move_Cmd`, `client/cl_input.c:612-615`):

1. **The hand ray.**
   - The weapon hand's aim pose comes from `VR_EngineBodyHand( VR_BODY_WEAPON )`
     (`vr/vr_input_engine.c:391`). It goes into the world with the shared convention:
     `world = render eye + R(body yaw) * offset` (MOVEMENT.md).
   - It is filtered: a speed-adaptive low-pass, about 1 € filter style. It is still at rest and
     follows fast motion with no lag. The laser uses the same filtered pose, so what is drawn is
     what is sent.
2. **The muzzle check.** Trace from the rendered eye to the hand muzzle (the gun's tip, about 20 cm
   ahead of the grip). If that is blocked, the ray starts at the blocking point. A gun pushed into a
   wall shoots that wall, as in single player.
3. **The hit point P** is the first hit of the hand ray, up to 8192 units:
   - against the world (`CM_BoxTrace`, model 0);
   - against the inline models of movers in the current snapshot (`CM_TransformedBoxTrace`);
   - against the players and other solid entities in the snapshot, as temporary boxes decoded from
     `entityState.solid`, at the positions the cgame draws. That means the same interpolation
     between the two snapshots around `cl.serverTime`.

   This is the engine's own copy of what `CG_Trace` does in the cgame. It uses only the data the
   cgame already has.
4. **The aim eye E** is the cgame's view origin with the bob removed (§4.4). That is the best
   estimate of the predicted `ps.origin + viewheight`, which is what the server's prediction-time
   eye will be.
5. **The world aim** is `vectoangles( P − E )`, with pitch clamped to ±87° and roll 0.
6. **The command** is `cmd.angles = ANGLE2SHORT( worldAim − SHORT2ANGLE( cl.snap.ps.delta_angles ) )`.
   The engine stores this in `cl.viewangles` so `CL_FinishMove` (`client/cl_input.c:571-584`) sends
   it unchanged. Everything else in the engine that reads `cl.viewangles` keeps working.

**Result:**
- The server's eye ray goes through P, so `CorrectForwardVector` aims the shot from its table muzzle
  at P. The shot lands where the hand points.
- If the line from the eye to P is blocked (the hand reaches around a corner the eye can't see past),
  the server's eye ray hits the corner, and so does the shot. **You can't shoot what your eye can't
  see.** That is fair by construction.
- The error is about the gap between E and the server's real eye, a few units or less, whatever the
  distance. A player's box is 30 units wide.
- It is parallax correction, not target selection. P is simply the first thing along the hand ray;
  nothing searches for targets.

### 3.3 The body yaw and the delta angles

- Today `VR_EngineInput` adds snap and smooth turns straight to `cl.viewangles[YAW]`
  (`vr/vr_input_engine.c:315-327`). In H1 `cl.viewangles` becomes the aim. So the turn goes into a
  new `vrBodyYaw`, kept in the same command space.
- **The world body yaw** is `vrBodyYaw + SHORT2ANGLE( cl.snap.ps.delta_angles[YAW] )`. When the
  server turns a player, on a teleport or a respawn (`SetClientViewAngle` changes `delta_angles`),
  the body turns with it, exactly as for a mouse player.
- **The hand's yaw** is taken relative to the body. A snap turn therefore swings the aim with the
  body, and others see a normal quick turn.

### 3.4 The pitch clamp

- `PM_UpdateViewAngles` (cmod:common/bg_pmove.c:2007-2033) clamps `cmd + delta` to ±16000 short
  units, which is ±87.9°. It does that by **rewriting `delta_angles[PITCH]`**.
- If a hand ever pointed past the clamp, the server would shift that player's pitch delta.
  Mouse-style input would then drift.
- H1 never lets that happen:
  - The world pitch is clamped at ±87° before the delta is subtracted, so the server never clamps.
  - Because every command is worked out from the latest snapshot's delta, even a delta the server
    changes on its own (on a respawn) is corrected on the next snapshot.
- Pointing straight down or straight up beyond 87° aims at 87°. The dot shows the true impact.

### 3.5 Roll

- Roll is always sent as 0, minus any roll delta.
- A sent roll would tilt the muzzle offsets and the movement axes (cmod:common/bg_pmove.c:2032,
  2178). It would also reach every client's view of the player through `apos`
  (cmod:common/bg_misc.c:1240).
- The wrist roll is drawn only on our own gun model (§4.3).

### 3.6 The hand and the eye

- The rendered eye is the cgame's eye plus the clamped head offset (§5). The hand is placed from the
  same anchor. So on screen the gun is exactly in the tracked hand, and the laser starts at its
  muzzle.
- **The tracer is the one honest mismatch.** Shots and beams start at the server's table muzzle,
  near the eye and about 25 units ahead. The engine moves the first-person beam starts to the hand
  (§4.3). Projectiles and predicted tracers from other events may still start a few units from the
  gun [guess on how visible; spike S3].

### 3.7 The laser and the dot

- **The laser:** a thin beam from the hand muzzle along the hand ray. It is drawn by the engine as a
  head-facing quad, like single player's `CG_VR_Beam`, with a built-in white shader, so pure servers
  need no extra pak.
- **The dot is the predicted real impact.** The engine repeats the server's maths with the angles it
  actually sends, after quantization:
  1. eye ray to Q;
  2. table muzzle (`WP_MuzzlePoint[]` for the current weapon, eye-to-muzzle check);
  3. a trace from the muzzle to Q.
- Usually Q is P. When Q isn't P (the eye can't see P, or something sits near the muzzle), the laser
  dims past Q and the dot sits at Q in a "blocked" colour. **The dot never promises a hit the server
  won't give.**
- No dot for the grenade launcher's arcing shots, only the line.
- The game's screen-centre crosshair is hidden (§4.5).
- The crosshair names still work. `CG_ScanForCrosshairEntity` traces along the cgame's view
  (cmod:cgame/cg_draw.c:2229-2232), which is now the aim through P.
- **The muzzle table (owner's decision, 2026-11-26: option 1).** The engine keeps a copy of the
  Holomatch game module's per-weapon muzzle offsets (`WP_MuzzlePoint[]`, cmod:game/g_weapon.c:2189-2204)
  as a small data table in `vr/vr_hm_aim.c`, with a comment saying it mirrors the game module's
  values (cMod by Noah Metzger / Chomenor, github.com/Chomenor/cmod-game-code). Nothing else from
  cMod or from Raven's game code is copied; the rest of the dot's maths is our own code.

### 3.8 Weapon prediction and lag compensation with fast hand aim

- **`cg_predictWeapons`.** cMod's prediction is active only when the cvar is on, the server supports
  it, and the player is predicted (cmod:cgame/cg_predict_weapons.c:38-46).
  - It repeats the server's muzzle maths from the **predicted player state**: the same table, the
    same eye and the same forward correction (:254-318).
  - The predicted state's angles come from our own latest command, so the predicted tracer and
    impact agree with the server and with our dot.
  - Fast aim changes nothing: each command carries its own angles.
- **`g_unlagged`.** It rewinds the **targets** to the shooter's `commandTime`, capped at 300 ms, using
  only their origins and boxes (cmod:game/mods/features/pingcomp/pc_instant_weapons.c:60-73,
  pc_position_shift.c:61-63). The shooter isn't moved.
  - Our P is worked out against the players at the positions the cgame draws at `cl.serverTime`.
    That is the same moment the rewind reconstructs, so the convergence and the rewind agree.
  - cMod's player smoothing (pc_smoothing.c) only shifts what is sent. We trace against what was
    received, exactly like the cgame.
- **Sampling.** Commands are built once per client frame (72 Hz on the Quest), with
  `cl_maxpackets` bundling as usual. A fast wrist flick is sampled no worse than a mouse at that
  frame rate.
- **Trigger pull.** Pulling the trigger moves the hand. The low-pass helps. A "hold the aim for 30 ms
  around the trigger edge" option is possible but off by default, because it is aim smoothing that
  flat players don't get [guess on whether it's needed].

---

## 4. Drawing without changing cMod's client game

### 4.1 Where the engine steps in

All of these are in `client/cl_cgame.c`, under `EF_VR` and `VR_HM_Active()`.

| Syscall | Line | What H1 does |
|---|---|---|
| `CG_R_ADDREFENTITYTOSCENE` | 783 | First-person entities (`renderfx & RF_FIRST_PERSON`) go into a small per-scene buffer instead of the renderer. So do beams (`RT_LINE`, `RT_ORIENTEDLINE`, `RT_LINE2`, `RT_BEZIER`, `RT_CYLINDER`, `RT_ELECTRICITY`; `renderercommon/tr_types.h:96-101`) and lights added during the view weapon's build. Everything else passes straight through. |
| `CG_R_ADDLIGHTTOSCENE` | 796 | A light at the flash origin is moved with the gun (§4.3). |
| `CG_R_RENDERSCENE` | 804 | For a world scene (no `RDF_NOWORLDMODEL`): read the cgame's view (`vieworg`, `viewaxis`), re-pose the buffered entities at the hand, add the laser and dot, then render. HUD scenes (`RDF_NOWORLDMODEL`) are untouched. |
| `CG_S_RESPATIALIZE` | 751 | The cgame passes its view as the listener (cmod:cgame/cg_view.c:987), which is now the gun's aim. The engine passes the **rendered head's** origin and orientation instead, so sound comes from the right direction. |

The cgame's own memory, its prediction and its effects are never touched. Only what it hands to the
renderer and the sound system is adjusted, and only on this client.

### 4.2 The view: origin and orientation

- `R_VR_SetupEye` (`renderervk/tr_vr.c:262-330`) takes the body yaw from the cgame's view axis
  (:274-282). In H1 that axis is the aim, so the world would turn with the gun.
- **Change:** in H1 the client publishes the world body yaw (§3.3), for example
  `VR_SetBodyYaw( yaw, valid )` in `vr/vr_openxr.c`. `R_VR_SetupEye` uses it when it is valid.
- Pitch and roll already come only from the headset (:284-298), so the cgame's aim pitch, damage
  kicks, the dead roll (cmod:cgame/cg_view.c:312-317) and the run pitch and roll are all ignored.
- **The origin:** the cgame's eye, with bob removed (§4.4), plus the clamped head offset (§5).
- **Zoom** (`+zoom`, cmod:cgame/cg_view.c:500-540) only changes `fov_x`/`fov_y`. VR draws with the
  headset's field of view (RENDERING.md), so zoom does nothing in VR. It isn't mapped. That is a
  small disadvantage, and a fair one.
- **Special states:**

  | State | Detected by | What VR shows |
  |---|---|---|
  | Following another player (`PMF_FOLLOW`) | `cl.snap.ps` | Theater screen (flat view on the big quad). That player's head turns would make a VR viewer ill. |
  | Intermission (`PM_INTERMISSION`) | `cl.snap.ps` | Theater screen, or stereo with the yaw frozen at its start. |
  | Dead | `STAT_HEALTH <= 0` | Stereo. The body yaw is frozen, the head is free, the cgame's 40° roll is ignored. |
  | Free spectator | `PM_SPECTATOR` | Stereo. The angles follow the **head**, not the hand, so flying goes where you look. Spectators can't shoot. |

### 4.3 The first-person weapon at the hand

**What the cgame draws** (cmod:cgame/cg_weapons.c):
- `CG_AddViewWeapon` (:1164-1258) builds a `hand` frame from the view and the bob, and marks it
  `RF_DEPTHHACK | RF_FIRST_PERSON` (:1255).
- `CG_AddPlayerWeapon` (:938-1156) adds the gun on the hands model's `tag_weapon`, then the barrels
  and the flash. They all copy the parent's `renderfx` (:958, 1083, 1118), so they all carry
  `RF_FIRST_PERSON`.
- The hands model itself is never added; it only carries tags.
- The powerup shells add the same gun again with a `customShader` (:868-920).
- The first-person phaser and dreadnought beam is an `RT_LINE`:
  - from `flash.origin` (:1147, fx_phaser.c:27-29);
  - to where the cgame's **eye ray** along the view hits (:778-782).

  With H1's angles, that endpoint is P.

**How the engine re-poses it**, at `CG_R_RENDERSCENE`:
1. **G** is the pose (origin and axis) of the first buffered `RF_FIRST_PERSON` model entity. That is
   always the gun.
2. **The target pose:** `G' = Hhand · Fit[weapon] · ( V⁻¹ · G )`.
   - V is the cgame's view frame.
   - `V⁻¹ · G` is the grip the game designed for a camera at the view.
   - `Hhand` is the filtered hand pose, roll included.
   - `Fit` is the per-weapon correction. Start from single player's `vr_weaponFit[]` and
     `cg_vrGunAdjust` values; the models are the same.
   - The weapon is identified by the gun's model name, through a model-name lookup the engine already
     has.
3. **T = G' · G⁻¹** is applied rigidly to every buffered first-person entity: gun, barrels, flash and
   shells.
4. **Beams** whose start lies within 2 units of the cgame's flash origin get T applied to the start
   only. Their end is the eye-ray hit, which is already right. **Lights** at the flash origin move
   with T.
5. **Clear `RF_DEPTHHACK`** on the re-posed entities. A depth hack in stereo draws the gun through
   walls; single player VR also draws it without one. `RF_FIRST_PERSON` stays, so mirrors and portals
   behave as before (`renderervk/tr_main.c:1686`).
6. **Bob and sway in the cgame's gun** (cmod:cgame/cg_weapons.c:627-672) cancel out:
   - V includes the view bob, and `V⁻¹ · G` keeps only the small weapon-only sway.
   - The idle drift is about ±0.4° at rest.
   - Option: strip the rotation part of `V⁻¹ · G` down to the hands-model tag, so the gun is rock
     still [spike S3].

**Testing guns:** `cg.testModelEntity` (cmod:cgame/cg_view.c:96) also uses `RF_FIRST_PERSON`. It's a
developer tool; re-posing it is harmless.

### 4.4 The cgame's view offsets in VR

`CG_OffsetFirstPersonView` (cmod:cgame/cg_view.c:293-410) adds:
- the bob height, up to 6 units, scaled by `cg_bobup` (:385-390);
- the landing dip, the step smoothing and the duck smoothing;
- kicks (:321-337, 407-409).

In VR:

| Offset | Handling |
|---|---|
| **Bob** | The engine sets `cg_bobup`, `cg_bobpitch` and `cg_bobroll` to 0 while H1 is active and puts the user's values back afterwards. All three are archived, non-cheat cvars in cMod (cmod:cgame/cg_main.c:175-177). In stock 1.2 they are believed to be the same [guess; spike S1 runs `cvarlist` against the stock cgame]. If a stock cgame marks them as cheat cvars, the engine doesn't force them. It removes the bob with a 2 Hz high-pass on the view origin's height instead. Restoring them must happen before the config is written (`CL_Shutdown`, a disconnect, a `vid_restart`), so a crash can't leave the user's flat profile changed. |
| **Steps, landing, kicks** | Kept. They are a few units and follow real events. |
| **Crouch** | The cgame drops the eye by the crouch height, and the real head drops too. In H1 a real crouch is sent as the crouch button (`VR_Move_Cmd`, `vr/vr_move.c:393-396`). The rendered eye's height is `min( cgame eye, standing eye + real head drop )`, and the standing eye is worked out with the engine's own duck ease from the commands it sends. So the drop is never doubled, and the eye is **never above the game's eye**: a crouched hitbox never peeks from standing height. |

### 4.5 The 2D HUD, the scoreboard, menus, chat and the console

All 2D already goes to the HUD and menu quad (RENDERING.md). In H1:

- **The crosshair.** The cgame's screen-centre crosshair would float in the middle of the HUD
  pointing nowhere. The engine sets `cg_drawCrosshair 0` while H1 is active, with the same
  save-and-restore as the bob cvars (archived in cMod, cmod:cgame/cg_main.c:158). The laser and dot
  replace it.
- **The HUD** (health, ammo, the team overlay): the lazy-follow quad as it is. The pillarbox bars are
  already skipped in VR (`client/cl_scrn.c:954`).
- **The scoreboard** (`+info`, cmod:cgame/cg_consolecmds.c:255) and the objectives (`+analysis`,
  :268): drawn by the cgame in 2D, so they show on the HUD quad as they are. Both need a button
  (§6).
- **Screen effects** (damage flashes, the zoom mask): on the quad. They are approximate but readable.
  Zoom isn't mapped.
- **The cMod UI and menus, and the console:** the UI catches keys, and RENDERING.md's menu frame
  applies (frozen, dimmed world and the world-locked menu quad). The existing right-stick cursor
  works.
  - Option for H1: **keep the world live, only dimmed, behind Holomatch menus**. The match goes on,
    and a frozen world hides danger. Proposed cvar `vr_menuLiveWorld`, default 1 in Holomatch
    [guess on comfort].
  - While the menu or console is up, the client sets `BUTTON_TALK` as usual
    (`client/cl_input.c:553-555`), and cMod zeroes movement and buttons
    (cmod:common/bg_pmove.c:2066-2075). Others see the talk balloon. That is normal.
- **Chat.** Reading chat works: it's 2D on the HUD. Typing needs a keyboard:
  - a Bluetooth keyboard works through SDL as on any Android device;
  - otherwise the Quest's system keyboard (an Android input-method editor) is an H1.x item;
  - until then, a few fixed messages can be bound (`say_team` binds) in the VR controls.

---

## 5. Head: rotation, position, steps and crouch

### 5.1 The rules (R2)

1. **The head's rotation drives only our rendering**, never the angles sent (§4.2). Others never see
   where a VR player looks; they see the gun's aim.
2. **The head's position is a view offset of at most `vr_hmLeanRadius`, default 4 units (12.5 cm),
   around the body's centre.** It is measured horizontally, inside the axis-aligned box. The drawn
   model's head is about that wide. So within the radius **the eye stays inside what other players
   see as the head**, and nothing is gained over a flat player.
3. **Past the radius, the whole body walks after the head** (§5.2). Leaning around a corner moves the
   real hitbox around the corner, and others see that player step out, honestly.
4. **Where the body can't follow, the eye stops at the radius and the view fades**, black about
   12 cm further:
   - a wall;
   - a ledge (below);
   - in the air;
   - a menu or chat open;
   - dead;
   - `vr_hmRoomScale 0`.

   The eye is never placed outside the radius, so there is never an unseen peek.
5. **The eye is never higher than the game's eye** (§4.4).

### 5.2 Walking the body through the usercmd

- Today, without the SP bridge, `VR_Move_Frame` drags the head origin along so the head stays within
  the radius, and nothing moves the body (`vr/vr_move.c:296-318`). In H1 that branch becomes:
  - The excess beyond the radius is a **step debt**, in metres in the room.
  - Each command turns the debt into `forwardmove`/`rightmove` with a proportional gain (about 8 per
    second). It is rotated into the **command's** yaw frame, the aim, not the body: movement is
    relative to `cmd.angles` (cmod:common/bg_pmove.c:2178).
  - At or below 64, `BUTTON_WALKING` is set, so footsteps are silent like a walking flat player.
    cMod clears the walk button above 64 (cmod:common/bg_pmove.c:2061-2064), so slow steps stay
    within its rule.
  - The head origin (the anchor) moves by what the body really achieved. That is measured from the
    cgame's eye moving horizontally between frames, and counted only along the commanded direction,
    only while the stick is idle. Knockback doesn't count.
  - The stick and room-scale add up. The stick's own direction rotation in `VR_Move_Cmd`
    (`vr/vr_move.c:376-387`) also has to move into the command's frame: today it assumes the command
    yaw is the body yaw.
- **Ledges:** before stepping, the engine traces the box down at the target spot
  (`CM_BoxTrace`, `MASK_PLAYERSOLID`, a step height plus 8 units). With no floor there, it refuses
  the step and fades instead. Leaning out to look down a drop must not throw the player off it.
- **Walls:** a small box trace from the body's eye to the rendered eye. The radius is inside the box,
  so it only catches the near plane in corners. It pushes the eye back, the same maths as
  `VR_Move_SPView` (`vr/vr_move.c:465-538`), with `CM_BoxTrace` in place of the SP trace.
- **Latency:** the step goes through normal prediction, so the body (and the eye) move on the next
  frame, locally. Others see it after the usual network delay.

### 5.3 Re-evaluating "clamp plus fade" against "lean moves the body"

| Option | Unseen advantage? | Honest to others? | VR player cost | Verdict |
|---|---|---|---|---|
| Free lean (no clamp) | **Yes:** sees round corners while the hitbox stays hidden | No | none | **Never** |
| Clamp at the box's half-width (11–15 units) plus fade | Small: the eye can be up to 15 units off the drawn head while the hitbox doesn't move | Partly | low | Rejected under R2 |
| **Small clamp (about 4 units) plus fade only** | None | Yes (nothing moves, nothing seen) | Medium: leaning stops dead and fades, so you have to walk | Fallback (`vr_hmRoomScale 0`), for seated play |
| **Small clamp, then the body walks** | None: the box steps out with the head | **Yes, visibly** | Low comfort cost, but you **expose yourself** when you peek | **Recommended default** |

The recommendation is the last row. It is the user's own example: peeking costs exposure, just as
strafing out does for a flat player. The clamp-and-fade only catches the cases where the body can't
move.

### 5.4 Crouch

- **A real crouch** (the head going `vr_crouchThreshold` below the standing height) and **the crouch
  button** both send `upmove −127` (`vr/vr_move.c:393-396`). Others see a crouch.
- The view follows §4.4: never above the game's eye.
- Standing up under a low ceiling: the game keeps the player crouched, and the eye stays at the game's
  crouched eye even if the real head rises.

---

## 6. Input, and what others see

### 6.1 The controller map for Holomatch

This extends `vr_map[]` (`vr/vr_input_engine.c:62-79`). The engine binds Holomatch's buttons
differently from single player: `+altattack` is button 5 (bit 32) and `+use` is button 6 (bit 64)
(`client/cl_input.c:1386-1393`).

The single-player "Use + strafe" trick in `VR_Move_Cmd` (`vr/vr_move.c:398-403`) must stay behind
`SP_IsActive()`. In Holomatch, bit 32 is alt fire.

| Control | In play |
|---|---|
| Right trigger | `+attack` (fire) |
| Left trigger | `+altattack` (alt fire, bit 32) |
| Right grip | `+button2` (use holdable item: transporter, medkit, detpack, forcefield, shield) |
| Left grip (hold) | `+info` (scoreboard) |
| A | `+moveup` (jump) |
| B | crouch (`VR_Move_CrouchButton`) |
| X | `+use` (bit 64: doors, buttons) |
| Y | `+analysis` (objectives), or the in-game menu |
| Right stick left/right | snap or smooth turn (body yaw) |
| Right stick up/down | `weapnext` / `weapprev` |
| Right stick click | the last weapon (`weapon <n>` of the previous one) |
| Left stick | move (in proportion to the deflection) |
| Left stick click | sprint toggle; held, recentre |
| Menu button | Escape |

- Gesture (`+button3`) and zoom aren't mapped.
- Chat binds are optional (§4.5).
- `vr_leftHanded` swaps the hands as in single player.

### 6.2 Comfort

- Snap turn by default, smooth turn optional, the vignette, and the wall and ledge fade, as in
  MOVEMENT.md.
- **Snap turns spread over 3 commands.** A 45° snap sends about 15° per command instead of one 45°
  jump. It looks the same in the headset (the view snaps at once) and is gentler on any server-side
  heuristic (§7.2) [guess on the value].
- The theater screen for spectating someone and for intermission (§4.2).

### 6.3 What other players see

| What the VR player does | What everyone else sees (stock or cMod cgame) |
|---|---|
| Looks around with the head | Nothing. The model faces the gun's aim (`apos` = view angles, cmod:common/bg_misc.c:1239-1240; head and torso, cmod:cgame/cg_players.c:1034-1087) |
| Points the gun | Head pitch = aim pitch; torso 0.75 of it, smoothed; torso and legs swing toward the aim yaw. A normal player aiming |
| Flicks the wrist fast | A quick mouse flick; the torso swing smooths it |
| Aims sideways while walking forward | A flat player strafing while aiming, with leg animation |
| Leans less than 12 cm | Nothing: the eye stays inside the drawn head |
| Leans further, or steps in the room | The player walks there, slowly and silently with the walk button, or at normal speed for big steps |
| Crouches for real | A crouch |
| Snap turns | A fast turn over about 40 ms |
| Holds the gun low or at arm's length | The usual third-person gun in the model's hand; the shots come from the usual muzzle beside the eye |

- **"Flick speed".** Human wrist turns peak at roughly 1,000°/s [guess]. Fast mouse flicks reach and
  pass that. Nothing in a hand's aim is faster or more exact than a mouse.
- **Fairness.** There's no target seeking. The convergence uses only the first thing along the hand
  ray. The eye stays within the drawn head. A shot can't come from a place the eye can't see. The
  slowed time of single player's weapon wheel never exists in Holomatch.

**Drawing VR players differently (R4): not in H1.**
- Our cgame isn't running on most servers: pure servers and stock modules impose their own.
- With no data channel, our clients can't tell VR players apart, and they have no head or hand pose
  to draw.
- Even with that data, a gun drawn in a VR player's real hand would show shots leaving from the hand
  while the server fires from the table muzzle beside the eye. A head turned to the real gaze would
  disagree with `apos`, which the shots follow. **Both mislead other players about where shots come
  from.** H2 (a server that really fires from the hand) is the only honest place for them.

---

## 7. Server-side checks: could a VR player be flagged or kicked?

### 7.1 cMod and stock 1.2 (read against cMod)

| Check | Where | Effect on VR |
|---|---|---|
| Pitch clamp | cmod:common/bg_pmove.c:2019-2031 | Silent, never a kick. H1 stays inside it (§3.4) |
| Command time sanity: −1000 / +200 ms | cmod:game/g_active.c:1856-1862 | H1 uses the engine's normal `serverTime` (`client/cl_input.c:578`) and adds no commands |
| Walk-button rule | cmod:common/bg_pmove.c:2061-2064 | H1's slow steps stay at or below 64 |
| Talk balloon zeroes input | cmod:common/bg_pmove.c:2066-2075 | Normal |
| Inactivity drop | cmod:game/g_active.c:463-495 | Only movement and attack count, not angles. Same as a flat player; room-scale steps count as movement |
| Userinfo | cmod:game/g_client.c:749-852 | `Info_Validate` checks syntax only (:764). Only known keys are read. Others receive only a configstring subset (:831-852) |
| Aim, angle-rate or "snap" heuristics | none found | A search of cMod's game code for cheat, kick and drop logic finds only `g_cheats` commands, the inactivity drop and the time clamp |
| Pure server | the engine's `cp` checksums of referenced paks | H1 adds **no pak content**. The laser uses a built-in shader. Nothing new is referenced |

### 7.2 Third-party server mods

- They may do things cMod doesn't. Server-side anti-aimbot heuristics (angle jumps tied to hits)
  aren't known to be common for Elite Force [guess].
- H1 lowers the risk anyway:
  - snap turns spread over several commands (§6.2);
  - the aim is filtered;
  - no angle ever goes outside the clamp, and roll is 0;
  - command timing is untouched;
  - no unusual userinfo.
- To test: a local dedicated server with any third-party mod the owner names, never a public server.

### 7.3 Engine-side checks on the server

- Holomatch servers can't see the client's engine. Protocol 24/26 has no binary check.
- Our flat clients already play on these servers. H1 adds only an input pattern.

### 7.4 A `vr` userinfo key, or another way to recognise each other

- **Harmless to stock servers:** an unknown key is ignored, and the 1,024-byte limit leaves room
  (`qcommon/q_shared.h:334`).
- **Useless between clients:** other clients never receive it (§7.1, the configstring subset).
- So by default it only labels the player in the server log and in `dumpuser`. That isn't wanted:
  R1 says the server never needs to know.
- **Recommendation:** off by default. A `vr_announce` cvar (default 0) adds `vr\1` only for H2
  negotiation with our own servers.
- **Other channels:**
  - A tiny roll in the sent angles would reach every client through `apos`, but it alters the
    simulation (the muzzle's right vector) and is exactly the kind of oddity a heuristic might notice.
  - Names, colours and models are visible to people.
  - **No honest, quiet channel exists without the server. None is recommended.**

---

## 8. What stays H2-only

- A hitbox that really leans (a server-side lean that shifts origin and box within limits).
- Shots from the hand (a hand muzzle, eye plus a clamped hand offset).
- Relaying head and hand poses, and drawing VR players' real hands, gun and gaze, consistent with
  the server.
- Any client-to-client recognition. Server negotiation via `vr_announce` or a modcfg string.
- VR-only server rules (free lean only when every client is VR).
- Zoom in VR (a magnified inset).

All of these need a cMod-based fork of the game modules on our servers, under Raven's licence, and
the cMod author's OK (PLAN.md, ground rule 4).

---

## 9. Implementation plan

### 9.1 Risks, and the spike that retires each one first

| # | Risk | Spike (flat Mac `EF_VR` build or the Meta XR Simulator; local server only) | Days |
|---|---|---|---|
| S1 | The stock 1.2 cgame differs from cMod: bob and crosshair cvar flags, `RF_FIRST_PERSON` on all view parts, beam starts | Run the stock and cMod cgames on a local server; `cvarlist cg_bob*`/`cg_drawCrosshair`; log every `CG_R_ADDREFENTITYTOSCENE` while firing each weapon (renderfx, reType, origin) | 1 |
| S2 | The view can't be re-based without breaking the HUD or prediction | `vr_debugHand` and `vr_debugHead` poses in Holomatch: the body yaw from the engine, the angles from a fake hand; check the HUD, prediction, the scoreboard and the menus with both cgames | 2 |
| S3 | The gun re-pose looks wrong: sway, flash, beams, shells | Re-pose with the fake wrist roll; capture each weapon firing, with quad damage on | 2 |
| S4 | The convergence misses: the eye estimate, the entity trace, delta handling | A bot standing still and a wall marker; fake hand poses at an offset; compare the dot with the scorch mark and the hit log, with `g_unlagged` 0/1/2 and `cg_predictWeapons` 0/1 | 2 |
| S5 | The room-scale walk feels bad or moves the body wrongly | A `vr_debugHead` sweep of 0.1 to 0.5 m beside a corner, with a flat observer client capturing; ledge and wall cases | 2 |

### 9.2 Steps and ownership

New work in new files, so merges stay easy.

| Step | Work | Files | Owner | Days [guess] |
|---|---|---|---|---|
| 0 | Spikes S1–S5 | debug code only, then dropped | vr-hm | 9 |
| 1 | `VR_HM_Active()`, the cvar save and restore (bob, crosshair), the special states | `vr/vr_hm.c` (new), `vr/vr_hm.h` | vr-hm | 2 |
| 2 | Body yaw against the aim, delta angles, the pitch clamp, roll 0; the renderer takes the body yaw | `vr/vr_hm_aim.c` (new); hooks in `client/cl_input.c:612-615`, `vr/vr_input_engine.c:315-327` (vr-move's turn code), `renderervk/tr_vr.c:274-282`, `vr/vr_openxr.c` (body-yaw getter) | vr-hm; small touches to vr-move and eye-render files | 4 |
| 3 | The engine trace: world, movers, snapshot players, interpolated | `vr/vr_hm_trace.c` (new), using `CM_*` and `cl.snapshots` | vr-hm | 4 |
| 4 | The convergence, the filter, the laser and the predicted-impact dot, the muzzle table | `vr/vr_hm_aim.c` | vr-hm | 3 |
| 5 | Scene interception: buffer, re-pose, beams and lights, no depth hack, `S_Respatialize` | `vr/vr_hm_scene.c` (new); hooks in `client/cl_cgame.c:751, 783, 796, 804` | vr-hm | 5 |
| 6 | The head: the 4-unit clamp, the step debt to usercmd, ledge and wall checks and fade, crouch height | `vr/vr_move.c:296-318` and `:355-405` (Holomatch branch) | vr-move's area: done by vr-hm with a minimal diff, or by vr-move | 5 |
| 7 | The Holomatch controller map, snap spreading | `vr/vr_input_engine.c:62-79, 217-336` | vr-hm (vr-move and quest-apk area) | 2 |
| 8 | The live world behind menus, theater for follow and intermission | `renderervk/tr_vr.c:200-215` | vr-hm (eye-render area) | 1 |
| 9 | Tests, the gates, docs (`docs/vr/HOLOMATCH.md`, the PLAN.md status) | | vr-hm | 4 |

**Total:** about 39 working days, **6–8 weeks** for one developer [guess]. The viability estimate of
3–5 weeks didn't count the engine entity trace and the scene interception.

**Order:** S1 → S2 → step 1–2 → S4 → steps 3–4 → S3 → step 5 → S5 → step 6 → steps 7–8 → step 9.
The aim (fairness, hits) comes first, then what the player sees, then comfort.

### 9.3 The test plan

**Setup:**
- Mac only, the Meta XR Simulator selected per launch with `XR_RUNTIME_JSON`, under the run lock,
  muted (`+set s_volume 0 +set s_musicvolume 0`).
- **Server:** our engine as a local dedicated server, `+set com_holomatch 1 +set dedicated 1`, on
  127.0.0.1, run twice:
  - with cMod (`g_unlagged 1`, `g_unlaggedPredict 1`);
  - with the stock 1.2 modules (`sv_pure 1`).
- **Clients:**
  - the VR client (simulator, `vr_debugHand`/`vr_debugHead` scripted poses);
  - a flat observer with the cMod cgame;
  - a flat observer with the stock cgame (cMod off);
  - a bot or two as targets.
- **Never join a public server.**

**Checks:**

| Test | Pass when |
|---|---|
| Aim accuracy | Scripted hand poses offset 0.2–0.4 m from the eye, aimed at wall markers and a standing bot from 100 to 1,500 units: the scorch or hit is within 3 units of the dot, for each hitscan weapon, with both `cg_predictWeapons` settings and `g_unlagged` 0/1/2 |
| Blocked eye | The hand reaches round a corner the eye can't see past: the dot shows "blocked" on the corner, and the shot hits the corner |
| Pitch clamp | The hand points straight up and down for 10 s: `cl.snap.ps.delta_angles[PITCH]` never changes (log) |
| Teleport and respawn | The body turns with the server; the aim stays relative to the hand |
| Observer view | Both flat observers capture the VR player: model facing the aim, no roll, normal animations; a 0.3 m lean beside a corner makes the model step out; a real crouch crouches; a snap turn looks like a quick turn |
| No unseen peek | Logged rendered eye against the server origin: never more than 4.1 units horizontally, never above the game eye |
| Ledge | Leaning over a drop doesn't move the body, and the view fades |
| HUD and UI | Scoreboard, objectives, chat text, console and cMod menus are readable; no centre crosshair; the sound comes from the head's direction |
| Server | Server log and `dumpuser`: no `vr` key, nothing unusual; a 20-minute session with no warnings or drops |
| Flat gates | `EF_COOP=0` `regress.sh check base-1_4_0` and the co-op `coop-m2-base`: **ALL IDENTICAL** |

**Evidence** in the notes folder. Clean up the builds and test homes afterwards.

### 9.4 Unverified until the spikes or a headset

- The stock 1.2 cgame's cvar flags and first-person entities (S1).
- How the gun's residual sway and the start points of predicted tracers look (S3).
- The comfort of the 4-unit radius and of room-scale walking (S5, then the Quest).
- Real controller jitter and the filter's settings (Quest).
- Third-party server mods (only with ones the owner names, locally).
