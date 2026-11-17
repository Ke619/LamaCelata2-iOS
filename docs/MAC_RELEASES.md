# Mac releases and the Ubisoft helper apps

Which Mac builds of AC2 and ACB exist, how ours compare with what Steam ships today,
and what the Ubisoft apps bundled with them do. First checked 2026-10-27 against
SteamDB file lists (SHA-1 and size per file); on 2026-11-10 both games were installed
through the Mac Steam client, AC2's 2012 manifest was downloaded with the Steam
console, and every file was compared with our copies.

## Steam depots

| Depot | Contents | Manifests seen by SteamDB | Our copy |
|---|---|---|---|
| 33362 | AC2 Mac game | `5654928643791563232` (2012-03-07), `4758459184864595027` (2014-06-16, current) | identical to the 2012 manifest, all 154 files; the current one differs in `scimitar_sf`, `Info.plist` and `version.plist` |
| 48193 | ACB Mac game, plus its Ubisoft folder | `3500960106426649477` (2014-06-16, the only one) | all 175 game-app files identical; the bundled launcher is another build (below) |
| 48172 | AC2's Ubisoft folder: launcher, GameUpdate, UPlay Browser | current manifest | all 150 files identical |

The apps' "last public update" of 2020-07-18 is an app-level build; neither game depot
changed then.

### The AC2 executable: Steam installs an older one

| | Steam's install today (2014 manifest) | Ours (2012 manifest) |
|---|---|---|
| Version | 1.0.0, build 25, source 285144 | 1.0.1, build 63, source 512707 |
| `scimitar_sf` | SHA-1 `f7177dd82e5c26efe14f95ccf0ce1bd0c03a8fde`, 79,741,016 bytes | SHA-1 `afcbbbf8046c47561b2a5a0574a11703071f15e5`, 92,989,696 bytes |
| Symbols | 68,740 (65,639 functions): mostly stripped | 308,449 (208,730 functions) |
| `__text` | 32,742,848 bytes | 32,754,496 bytes |

All game data (every `.forge`, voice pack and video) is the same in both. The 2014
manifest changes only the executable (with its `Info.plist` and `version.plist`),
and it went back to the older 1.0.0. 1.0.1 is
built with a newer SDK (`$INODE64` file calls), detects the system language
(`NSLocale`), picks display modes by refresh rate, follows view resizes, and links
OpenSSL 0.9.8 and the Security framework. 1.0.0 draws through an `NSOpenGLView`.

The port is built from 1.0.1 and needs its symbols: of the game functions it
replaces or looks up by name, most are missing from 1.0.0. The port loads the
executable from the `.app` it is given, so 1.0.0 does not work with it.

With the game owned, the Steam console still fetches 1.0.1 (verified 2026-11-10;
33230 is AC2's app ID, 33362 its Mac depot):

```
download_depot 33230 33362 5654928643791563232
```

Steam reports 181 files (154 files and 27 folders, 7,486 MB) and, when done, the
folder it wrote to: on the Mac client
`~/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steamapps/content/app_33230/depot_33362/`,
which holds `Assassin's Creed 2.app`.

### ACB

ACB 1.0.0 (build 1.0.0.19, source 378274) matches the only manifest file by file,
including both executables, the Orbit loader and every archive. A plain install from
the Mac Steam client is identical to our copy (all 175 files of the app).

The launcher in our ACB folder is build `0327`; a Steam install today brings `0266`. 61 of its 65 files are identical to
depot 48172's launcher (`0331`): it lacks `installscript_osx.vdf`, and
`UbisoftGameLauncher`, `product_config_schema_1_5.yml` and `version.txt` differ.
Depot 48193's own launcher listing is smaller still (74 entries against our 99,
without QtDBus, QtNetwork, QtWebKit, QtXml, phonon or the sound effects). Ours most
likely came from the launcher updating itself. The port does not use it.

## The Ubisoft helper apps

All three are 32-bit Intel, which macOS has not run since 10.15, and all depend on
Ubisoft online services of the time.

### GameUpdate.app

Ubisoft's Mac patcher ("Evil GameUpdate"); AC2 and ACB ship the same binary.

- At startup the game checks for
  `/Library/Preferences/com.ubisoft.AssassinsCreed2.GameUpdate.plist`
  (`MacCheckUpdaterLaunch`, called from `scimitar::main`). If it exists, the game
  launches GameUpdate through `MacLaunchBundle` and steps aside. Otherwise it skips
  the updater, as it does in the port.
- GameUpdate reads `info` (the game ID), `installdir`, `execPath` and optionally
  `xmlURL` from that preferences domain. It fetches
  `http://gconnect.ubi.com/MatchMakingConfig.aspx?action=g_guc&gid=<info>&locale=en-US`,
  an XML patch description (`Version=`, `Signature=`, `Size=`, `Mirrors:`,
  `PatchCRC`, `previousCRC`), then downloads, CRC-checks and unpacks the patch.
- Only Ubisoft's own installer wrote those preferences. The Steam install scripts
  never do, so Steam copies never ran GameUpdate: Steam was their updater.
- The game ID is a 128-bit token, not something to guess. The GOG Prince of Persia
  install script shows the same scheme on Windows
  (`HKLM\SOFTWARE\Ubisoft\Prince of Persia\GameUpdate`, `info` =
  `08a0f1c01d540ee143f7ee48a91898fa`). The Mac IDs of AC2 and ACB are not known.

### UPlay Browser.app

The client for the old Uplay rewards: a points store of unlockable extras (Scaleform
UI, `UPlayMarketplace`). `scimitar::LoadUplayBrowser()` launches it through
`MacLaunchBundle` and then shuts the game down. ACB's install script records its
path under `com.Ubisoft.AssassinsCreedBrotherhood.UPlay` / `execPath`.

### Ubisoft Game Launcher.app

A Qt launcher from Ubisoft's `uplay_pc/1.5` code base: account sign-in, CD-key
binding, cloud saves, reward downloads, a "game version banned" check and its own
self-update. It has no store or game library and cannot download or install games.
It ships `ubiorbitapi_r2.dylib`, the Orbit library the port replaces (see
[PLAN.md](PLAN.md), Decisions). Ubisoft Connect has no Mac client, so these Mac
builds are not available through a Ubisoft account; Steam (including older
manifests) is the source.

## Open questions

- How AC2 decides Uplay reward unlocks. If it asks through the Orbit calls the port
  already answers, the local Orbit code could unlock them.
