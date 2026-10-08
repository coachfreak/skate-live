# How Skate.live works

| Part | File | Job |
|---|---|---|
| Library | `dylib/lan.cpp` | Injected into skate3recomp. It turns on the recomp's Xbox networking, which ships switched off. It covers Live sign-in and privileges, the LAN IP as the Xbox address, Xbox addresses mapped to IPs, DNS (`*.ea.com` to the server), Winsock-style socket errors, `select`, `getsockname` and `getsockopt`. It also hooks `BlazeHub::initialize` (`sub_829953A0`) to set plain TCP and point the redirector at the server. |
| Server | `server/skate3_lan_server.py` | Python, standard library only. Blaze on TCP 42127 (redirector, preAuth, xboxLogin, postAuth, user sessions, game manager) and HTTP on port 80 for `/skate3/config/XBL2.xml`. `--test` runs the TDF codec self-check. |
| Launcher | `launcher/Skate3LAN.swift` | Menu in the game's style (`launcher/SkateStyle.swift`): mode, name, address, server and logs. Starts the server and the game with the library (`SKATE3_LAN_SERVER`, `SKATE3_LAN_NAME`, `SKATE3_LAN_XUID`). Icon glyph: `launcher/make_glyph.swift`. |
| Server app | `server_app/` | Runs only the server, on this Mac's Tailscale IP, in a Terminal window. |
| Title logo | `tools/title_logo/` | Regenerates `dylib/title_logo.h`, the press-start logo redrawn as "skate.live", from the installed game's `fedynamic.big`. |

Blaze message shapes follow Hall of Meat's [Skateboard3Server](https://github.com/hallofmeat/Skateboard3Server) (MIT, PS3). The Xbox 360 command and notification ids were read from the game itself (`sub_829CFD68` / `sub_829D20C8`, the name lookups). For example, 360 login is `xboxLogin` (auth `0xAA`), not PS3's `0xC8`.

Gameplay traffic goes directly between players, using the game's own DirtySock networking over plain UDP. The server only handles login and matchmaking.

## Friends

There is no adding friends: everyone the server has seen is everyone else's friend. The server remembers every player who logged in or whose Join launcher found it (`players.json` beside the logs). That's needed because the game fetches its friend list (Blaze `friendList`) only once, at login, and has no "friend added" message. A friend who joins later is therefore already on the list, shown offline, and turns online when they log in. The Xbox friends check always says yes. Joining a friend's game by player (Blaze `joinGame` with `USER`) also works.

## What the runtime gets wrong and `lan.cpp` replaces

- no network link, DNS or Xbox address
- Winsock error codes
- `select`, `getsockname` and `getsockopt`
- overlapped receive (`WSARecvFrom`, `WSAGetOverlappedResult`): DirtySock's socket thread waits on them forever
- `sendto`, which sent to byte-swapped addresses

## Testing two games on one Mac

Give each game its own address so both can bind port 10000. Also give each its own name, XUID, log and input file. The input file holds one pad press per line, e.g. `start`, `a`, `rt`, `wait 1000`:

```sh
cd /Applications
DYLD_INSERT_LIBRARIES=<repo>/build/libskate3lan.dylib SKATE3_LAN_SERVER=127.0.0.1 \
SKATE3_LAN_IP=127.0.0.1 SKATE3_LAN_NAME=ClientB SKATE3_LAN_XUID=0009BBBB00000002 \
SKATE3_LAN_LOG=/tmp/b.log SKATE3_LAN_INPUT=/tmp/b.in \
./skate3recomp.app/Contents/MacOS/skate3 --skate3_demo_path=true --skate3_demo_path_signed_in=true &
```

The other game uses the Mac's LAN IP for `SKATE3_LAN_SERVER` / `SKATE3_LAN_IP`. Don't start it through `nohup` or `env`: macOS strips `DYLD_*` variables when it runs a system binary.

## Known issue: intermittent black screen at startup

This also happens with the unmodified game started directly from Terminal, without Skate.live.

- **What the logs show:** In the black starts, the runtime created the GPU system but never logged the start of host thread 3 (`GPU Commands`); successful starts do. The render thread was stuck waiting for graphics command progress (`sub_82B82E08` → `sub_82B755C0` → `sub_82B76080`).
- **Likely cause:** In `rexglue-sdk/src/core/threading_posix.cpp`, `ThreadStartRoutine` publishes the suspended state before it sets `suspend_count_ = 1`. `Resume()` can then see the state change, observe a zero count and return, after which the worker sets the count and waits forever.
- **Fix:** Publishing the count before the state fixes it. The official 2.0.2 binary still contains the race, so it needs a rebuilt runtime.
- **Workaround:** Quit and start again.

A separate crash seen once, an `InvalidFunctionTrap` from `sub_82B48C48` on the render thread, isn't linked to Skate.live by the available traces.
