# Co-op compatibility across releases

Since 1.5.1, two copies play co-op together when they share **two numbers**, whatever app
version each one runs:

| Number | Where | What it covers |
|---|---|---|
| `VSP_COOP_PROTOCOL` (= `LC_COOP_PROTOCOL`) | `efcode/sys/sys_coop_net.h`, `efcode/launcher/lc_coop.h` | The co-op messages: their kinds, keys and fields |
| `VSP_COOP_COMPAT` (= `LC_COOP_COMPAT`) | the same two headers | Everything else both copies must agree on, named after the first release that has it, e.g. `"1.5.0"` |

`EF_APP_VERSION` (1.5.1, 1.5.2, and so on) is only shown to players, never compared. So a
patch release that changes nothing co-op depends on still pairs with the releases before it.

## On the wire

Every co-op message (the host's `coopInfoResponse`, the guest's `coopConnect`) carries:

- `coopProtocol`: `VSP_COOP_PROTOCOL`
- `version`: `VSP_COOP_COMPAT`. This is the key compared.
- `appVersion`: `EF_APP_VERSION`. It is only shown: in the sheet's "Host's app" line and in
  "another version" messages.

Copies from before 1.5.1 send no `appVersion`, and their `version` is their app version. A
1.5.0 copy compares `version` with its own `"1.5.0"`, so 1.5.1 and later 1.5.x releases (all
with compat `"1.5.0"`) pair with 1.5.0 in both directions. Where `appVersion` is missing, the
shown version falls back to `version`.

## When to bump

| What changed | Bump |
|---|---|
| A co-op message (`sys_coop_net.c`, `sys_coop_host.c`, `sys_osx_coop.m`, `lc_coop.c`): a new kind, key or field, or a changed meaning | `VSP_COOP_PROTOCOL` and `LC_COOP_PROTOCOL` (+1), and the compat version |
| What the replica copies byte for byte: `gentity_t`, `gclient_t`, `gNPC_t`, `parms_t`, `level_locals_t` up to `LEVEL_LOCALS_T_SAVESTOP`, `camera_t`, `playerState_t`, `entityState_t`, `usercmd_t`, the replica's own `rep*_t`, the save field tables (`g_savegame.cpp`) | The compat version |
| `bg_*` movement code (the guest predicts player 2 with its own copy, the host runs it for real) | The compat version |
| The engine/game co-op bridge (`coop_bridge.h`) in a way the other copy sees, or the replica's frame format | The compat version (and the protocol if the messages changed) |
| A new major.minor line (1.6.0) | The compat version, to the new version: the build refuses otherwise |
| Host-only game logic: AI, scripts, most of `efgame/src/coop` (Use, teleport, revive, pings) | Nothing. The fix works when the **host** has it, whatever the guest runs |
| Guest-only code: input, sound, rendering, HUD, launcher look | Nothing |

To bump the compat version, set `VSP_COOP_COMPAT` and `LC_COOP_COMPAT` to the version being
released, e.g. `"1.5.3"`. From then on, 1.5.3 and later pair with each other but not with 1.5.2
and earlier, and the sheet says so ("Another version: the host runs Elite Force 1.5.2 ...").

## The guards

- **Configure time** (`engine/CMakeLists.txt`): the two compat defines and the two protocol
  defines must match, and the compat version's major.minor must match `EF_APP_VERSION`'s.
  Configuring 1.6.0 with compat `"1.5.0"` stops with what to change.
- **Build time** (`efgame/src/coop/coop_replica.cpp`): `static_assert`s on the size of every
  struct the replica copies. A changed size fails the build and asks for the bump, then the new
  number. The sizes are the same on macOS arm64, Windows x64 and Linux x86_64. Sizes don't catch
  a reordered field or a same-size type change: those need the bump too.
- **CI** (`tools/check-coop-compat.sh`, job `coop-compat`): compares the tree with the last
  `macos-v*` tag. If a file in the table above changed and neither number moved, it fails, unless
  a commit since the tag explains why with a trailer line:

  ```
  Coop-Compat: unchanged (the handshake now compares the compat version; the messages are the same)
  ```

  The file list is broad on purpose (`g_local.h` holds SP-only fields too), so expect to write the
  trailer for most single-player fixes that touch those headers.
