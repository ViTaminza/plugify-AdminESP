# AdminESP (Plugify)

Player outlines for admin_system admins, sent only to them: every living player gets an invisible bone-merge relay and a glowing prop_dynamic copy whose transmit bits are cleared for everybody except an admin whose ESP is on (and whom admin_flag_all/admin_flag_death allow - always, or only while dead or spectating). ESP is off until an admin types !esp / mm_esp or uses the "players" admin menu item; the viewer's client is also told mp_forcecamera 0 and mp_playerid 0 while the outlines are on. Config and phrases keep the Metamod names: adminesp.ini and AdminESP.phrases.txt. A port of the Metamod:Source plugin 'AdminESP'.

Original plugin (not a port).

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, admin_system, hudmenu (optional)

## Installation
Download `AdminESP-2.4.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/AdminESP`.

`plugins/admin_system` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

