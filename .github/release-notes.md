**AwayTeamEF @VERSION@**: *Star Trek: Voyager – Elite Force* for **Mac, Windows and Linux** (Steam Deck too), with **co-op for the single-player campaign**, Holomatch multiplayer and the full single-player game. Formerly "Elite Force (Marino's Build)".

> **No game data is included.** You need your own copy of the game (GOG, Steam, or CD + 1.2 patch).

### Downloads
| Platform | File | How to start |
|---|---|---|
| **Mac** (Apple Silicon) | **EliteForce-MacOS-Marino-@VERSION@-arm64.dmg** (or the .zip) | Drag the app to Applications and open it. Signed and notarized by Apple. |
| **Windows** (64-bit) | **EliteForce-Marino-@VERSION@-setup.exe** (installer) or **EliteForce-Marino-@VERSION@-windows-x64.zip** | Run the installer, or unzip anywhere and run EliteForce.exe. **Not code-signed yet:** if SmartScreen warns, choose "More info", then "Run anyway". |
| **Linux** (x86_64, Steam Deck too) | **EliteForce-Marino-@VERSION@-x86_64.AppImage** | `chmod +x` it and run it. |

> **Windows:** if Windows reports "Bad Image … 0xc0e90002", Smart App Control is blocking the unsigned build. Code signing will be in a future build. You may have to search for how to work around this.

The launcher on each platform helps you point the game at your data. **Every platform plays co-op with every other**: a Mac can host a Windows PC or a Steam Deck, and the other way round. Both players need 1.5.0 or later: 1.5.1 plays co-op with 1.5.0.

## What's new in @VERSION@

### Highlights
- **Install from your CD.** Choose… and Add Game Files… accept an image of the game's CD or the Expansion Pack's CD (.iso, .bin/.cue, .img), alongside a BaseEF folder and GOG's setup .exe. The launcher copies the game files straight out of the image.
- **Controls buttons in the launcher.** **Use Recommended** sets WASD, mouse aim and fire, E to use, Space/X to jump, F5 quicksave and more. **Restore Original** brings back the game's own keys. Both change single player and Holomatch at once, and leave controller bindings alone.
- **Your keys and settings follow you onto mod servers.** Joining a Holomatch server that runs a mod (tw2, nanoef, EFAdmin, …) used to bring up an old copy of your keys and graphics. Now your main keys and engine settings apply there, and changes you make there stick.
- **A new name in the game, too.** The console and the menus show "AwayTeamEF for Mac 1.5.1" (or Windows, Linux).

### Co-op
- **Leaning no longer teleports you.** Lean is Use plus strafe, so a guest who leaned for a few seconds was brought to the host, and two quick leans sent a ping. Holding Use on its own still teleports.
- **1.5.1 plays with 1.5.0.** Copies now pair by co-op version, so a patch release no longer splits players.

### Single player & everyone
- **The tricorder's scan sound** no longer piles up while the button is held.
- **The launcher's graphics settings** come from whichever mode you played last, so a setting such as Full Retina resolution no longer shows unchecked after you turned it on in the other mode.

### Holomatch
- **The server browser counts people** on servers that don't report them, instead of showing "?". Bots no longer make a server look busy under "Only servers with people".
- **Server admins can spot AwayTeamEF players** with `dumpuser`. Other players don't receive this, and vanilla servers ignore it.

### Linux & Steam Deck
- **Set Steam Deck Controls** sits in the launcher's Controls group, and Use Recommended sets the Deck's controller layout too.

Thanks to **Noah Metzger (Chomenor)** for cMod's game modules, used by Holomatch.
