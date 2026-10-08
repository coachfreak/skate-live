#!/usr/bin/env python3
"""Skate 3 LAN server.

Stands in for EA's Skate 3 online servers on a local network, for skate3recomp
games started by the Skate.live launcher (dylib/lan.cpp points them here):
  - Blaze, TCP 42127: redirector, login, user sessions, and a game manager that
    puts everyone in the first player's online freeskate. Plain TCP, no SSL.
  - HTTP, TCP 80: the game's online config (/skate3/config/XBL2.xml).
  - Discovery, UDP: a beacon broadcast to port 42128 every second (launchers in
    Join mode fill in this address), and port 42129 for launchers saying hello.
No EA account: whoever connects is logged in with their recomp profile's gamertag.

Message shapes follow Hall of Meat's Skateboard3Server (MIT, PS3 only); the
Xbox 360 login (xboxLogin) and all command / notification ids were read from the
360 client. Every request and reply is logged decoded, so a log from a play
session shows what the game asked for.

    python3 skate3_lan_server.py [--ip LAN_IP] [--log FILE] [--name HOST] [--players FILE]
    python3 skate3_lan_server.py --test        # codec self-check
"""

import argparse
import asyncio
import http.server
import itertools
import json
import socket
import struct
import sys
import threading
import time

BLAZE_PORT = 42127
HTTP_PORT = 80
BEACON_PORT, HELLO_PORT = 42128, 42129  # discovery: server -> launchers, launchers -> server
EN_US = 0x656E5553

# ---------------------------------------------------------------- TDF
# Blaze's tagged binary format, the variant Skate 3 (BlazeSDK 2.x) speaks: a 3-byte
# tag (4 chars x 6 bits), a type/length byte (length nibble 0xF: VLQ length
# follows), then the value. Integers are fixed-size big-endian; structs end at 0x00.

STRUCT, STRING, INT8, UINT8, INT16, UINT16, INT32, UINT32, INT64, UINT64, ARRAY, BLOB, MAP, UNION = range(14)
INT_SIZE = {INT8: 1, UINT8: 1, INT16: 2, UINT16: 2, INT32: 4, UINT32: 4, INT64: 8, UINT64: 8}
TYPE_NAME = "struct string i8 u8 i16 u16 i32 u32 i64 u64 array blob map union".split()


class Int:
    __slots__ = ("type", "value")

    def __init__(self, type_, value):
        self.type, self.value = type_, value

    def __repr__(self):
        return f"{TYPE_NAME[self.type]}({self.value})"


class Struct(list):
    """[(tag, value), ...]; value is Int, str, bytes, Struct, Array, Map or Union."""

    def get(self, tag, default=None):
        for t, v in self:
            if t == tag:
                return v.value if isinstance(v, Int) else v
        return default


class Array:
    def __init__(self, etype, items):
        self.etype, self.items = etype, list(items)  # int elements are plain ints


class Map:
    def __init__(self, ktype, vtype, items):
        self.ktype, self.vtype, self.items = ktype, vtype, list(items)  # [(key, value)]


class Union:
    def __init__(self, key, value=None):
        self.key, self.value = key, value  # key 0x7F = unset


def i8(v): return Int(INT8, int(v))
def u8(v): return Int(UINT8, v)
def u16(v): return Int(UINT16, v)
def i32(v): return Int(INT32, v)
def u32(v): return Int(UINT32, v)
def i64(v): return Int(INT64, v)
def u64(v): return Int(UINT64, v)
def S(**fields): return Struct(fields.items())


def tdf_type(v):
    if isinstance(v, Int): return v.type
    if isinstance(v, Struct): return STRUCT
    if isinstance(v, str): return STRING
    if isinstance(v, (bytes, bytearray)): return BLOB
    if isinstance(v, Array): return ARRAY
    if isinstance(v, Map): return MAP
    if isinstance(v, Union): return UNION
    raise TypeError(f"no TDF type for {v!r}")


def vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append(0x80 | (n & 0x7F))
        n >>= 7
    return bytes(reversed(out))


def tag_code(tag):
    v = 0
    for i, c in enumerate(tag):
        v |= (0x20 | (ord(c) & 0x1F)) << (3 - i) * 6
    return v


def header(t, length):
    return bytes([t << 4 | length]) if length < 0xF else bytes([t << 4 | 0xF]) + vlq(length)


def body(t, v):
    """(length for the header, payload) of a value of type t (int elements as plain ints)."""
    if t == STRUCT:
        return 0, encode(v) + b"\0"
    if t == STRING:
        b = v.encode("latin1") + b"\0"
        return len(b), b
    if t in INT_SIZE:
        n = v.value if isinstance(v, Int) else v
        return INT_SIZE[t], (n & ((1 << 8 * INT_SIZE[t]) - 1)).to_bytes(INT_SIZE[t], "big")
    if t == BLOB:
        return len(v), bytes(v)
    if t == ARRAY:
        out = vlq(len(v.items))
        for i, item in enumerate(v.items):
            length, payload = body(v.etype, item)
            if i == 0:
                out += header(v.etype, length)
            elif v.etype in (STRING, BLOB):
                out += vlq(length)
            out += payload
        return 1, out
    if t == MAP:
        out = b""
        for i, (k, val) in enumerate(v.items):
            for et, x in ((v.ktype, k), (v.vtype, val)):
                length, payload = body(et, x)
                if i == 0:
                    out += header(et, length) if et == STRUCT else bytes([et << 4 | 0xF]) + vlq(length)
                elif et in (STRING, BLOB):
                    out += vlq(length)
                out += payload
        return len(v.items), out
    if t == UNION:
        out = bytes([v.key])
        if v.value is not None:
            out += field("VALU", v.value)
        return 0, out
    raise TypeError(t)


def field(tag, v):
    t = tdf_type(v)
    length, payload = body(t, v)
    return tag_code(tag).to_bytes(3, "big") + header(t, length) + payload


def encode(fields):
    return b"".join(field(tag, v) for tag, v in sorted(fields, key=lambda f: tag_code(f[0])))


class Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def take(self, n):
        if self.pos + n > len(self.data):
            raise ValueError("truncated TDF")
        b = self.data[self.pos:self.pos + n]
        self.pos += n
        return b

    def vlq(self):
        n = 0
        for _ in range(8):
            b = self.take(1)[0]
            n = n << 7 | (b & 0x7F)
            if not b & 0x80:
                return n
        raise ValueError("bad length")

    def header(self):
        b = self.take(1)[0]
        return b >> 4, (self.vlq() if b & 0xF == 0xF else b & 0xF)


def tag_text(code):
    return "".join(chr(0x40 | (c & 0x1F)) for c in ((code >> (18 - 6 * i)) & 0x3F for i in range(4)) if c)


def read_value(r, t, length):
    if t == STRUCT:
        r.take(length)
        return read_fields(r, nested=True)
    if t == STRING:
        return r.take(length).rstrip(b"\0").decode("latin1")
    if t in INT_SIZE:
        return Int(t, int.from_bytes(r.take(length), "big", signed=t in (INT8, INT16, INT32, INT64)))
    if t == BLOB:
        return bytes(r.take(length))
    if t == ARRAY:
        count, items, etype = r.vlq(), [], STRUCT
        for i in range(count):
            if i == 0:
                etype, elength = r.header()
            elif etype in (STRING, BLOB):
                elength = r.vlq()
            items.append(element(r, etype, elength))
        return Array(etype, items)
    if t == MAP:
        items, ktype, vtype = [], STRING, STRING
        for i in range(length):
            if i == 0:
                ktype, klength = r.header()
                k = element(r, ktype, klength)
                vtype, vlength = r.header()
            else:
                k = element(r, ktype, r.vlq() if ktype in (STRING, BLOB) else klength)
                if vtype in (STRING, BLOB):
                    vlength = r.vlq()
            items.append((k, element(r, vtype, vlength)))
        return Map(ktype, vtype, items)
    if t == UNION:
        r.take(length)
        key, value = r.take(1)[0], None
        if r.data[r.pos:r.pos + 3] == tag_code("VALU").to_bytes(3, "big"):
            r.take(3)
            value = read_value(r, *r.header())
        return Union(key, value)
    raise ValueError(f"unknown TDF type {t}")


def element(r, t, length):
    v = read_value(r, t, length)
    return v.value if isinstance(v, Int) else v


def read_fields(r, nested=False):
    out = Struct()
    while r.pos < len(r.data):
        if nested and r.data[r.pos] == 0:
            r.pos += 1
            return out
        tag = tag_text(int.from_bytes(r.take(3), "big"))
        out.append((tag, read_value(r, *r.header())))
    return out


def decode(data):
    return read_fields(Reader(data))


def show(v, indent=1):
    pad = "  " * indent
    if isinstance(v, Struct):
        return "".join(f"\n{pad}{tag} = {show(x, indent + 1)}" for tag, x in v) or "{}"
    if isinstance(v, Array):
        if v.etype in INT_SIZE or v.etype == STRING:
            return f"[{', '.join(map(repr, v.items))}]"
        return "".join(f"\n{pad}- {show(x, indent + 1)}" for x in v.items) or "[]"
    if isinstance(v, Map):
        return "{" + ", ".join(f"{k!r}: {show(x, indent + 1)}" for k, x in v.items) + "}"
    if isinstance(v, Union):
        return f"union {v.key}" + ("" if v.value is None else f" {show(v.value, indent)}")
    if isinstance(v, (bytes, bytearray)):
        return f"blob[{len(v)}] {bytes(v[:48]).hex()}{'...' if len(v) > 48 else ''}"
    return repr(v)


# ---------------------------------------------------------------- names (from the 360 client)
COMPONENTS = {1: "auth", 4: "gamemanager", 5: "redirector", 7: "stats", 9: "util", 0xB: "teams", 0xC: "gamereporting",
              0xF: "messaging", 0x19: "associationlists", 0x7802: "usersession"}
COMMANDS = {
    1: {0x20: "getEntitlements", 0x26: "listEntitlement2", 0x28: "login", 0x46: "logout", 0x6E: "loginPersona",
        0xAA: "xboxLogin", 0xC8: "ps3Login", 0xE6: "createWalUserSession"},
    4: {0x1: "createGame", 0x2: "destroyGame", 0x3: "advanceGameState", 0x4: "setGameSettings", 0x5: "setPlayerCapacity",
        0x7: "setGameAttributes", 0x8: "setPlayerAttributes", 0x9: "joinGame", 0xA: "updatePlayerConnection",
        0xB: "removePlayer", 0xD: "startMatchmaking", 0xE: "cancelMatchmaking", 0xF: "finalizeGameCreation",
        0x10: "updateHostConnection", 0x11: "listGames", 0x19: "resetDedicatedServer", 0x1A: "updateGameSession",
        0x1D: "updateMeshConnection"},
    5: {0x1: "getServerInstance"},
    7: {0x8: "updateStats"},
    9: {0x1: "fetchClientConfig", 0x2: "ping", 0x3: "setClientData", 0x5: "getTelemetryServer", 0x6: "getTickerServer",
        0x7: "preAuth", 0x8: "postAuth", 0xA: "userSettingsLoad", 0xB: "userSettingsSave", 0xC: "userSettingsLoadAll",
        0x15: "fetchQosConfig", 0x16: "setClientMetrics", 0x17: "setConnectionState"},
    0xC: {0x1: "submitGameReport", 0x2: "submitGameEvents"},
    0x19: {0x5: "getListForUser", 0x6: "getLists"},
    0x7802: {0x1: "lookupUserInformation", 0x3: "fetchExtendedData", 0x4: "updatePingSiteLatency",
             0x5: "updateExtendedDataAttribute", 0x8: "updateHardwareFlags", 0xC: "lookupUser", 0xD: "lookupUsers",
             0x14: "updateNetworkInfo"},
}
NOTIFY_MATCHMAKING_FINISHED, NOTIFY_GAME_REMOVED, NOTIFY_JOIN_GAME, NOTIFY_PLAYER_JOINING = 0x0A, 0x10, 0x14, 0x15
NOTIFY_PLAYER_JOIN_COMPLETED, NOTIFY_PLAYER_REMOVED, NOTIFY_GAME_ATTRIB_CHANGE = 0x1E, 0x28, 0x50
NOTIFY_GAME_STATE_CHANGE, NOTIFY_GAME_SETTINGS_CHANGE = 0x64, 0x6E
NOTIFY_EXTENDED_DATA, NOTIFY_USER_ADDED, NOTIFY_USER_REMOVED = 0x1, 0x2, 0x3  # usersession
NOTIFY_NAMES = {(4, 0x0A): "NotifyMatchmakingFinished", (4, 0x10): "NotifyGameRemoved", (4, 0x14): "NotifyJoinGame",
                (4, 0x15): "NotifyPlayerJoining", (4, 0x1E): "NotifyPlayerJoinCompleted",
                (4, 0x28): "NotifyPlayerRemoved", (4, 0x50): "NotifyGameAttribChange",
                (4, 0x64): "NotifyGameStateChange", (4, 0x6E): "NotifyGameSettingsChange",
                (0x7802, 1): "UserSessionExtendedDataUpdate", (0x7802, 2): "UserAdded",
                (0x7802, 3): "UserSessionUnsubscribed", (0xC, 0x72): "ResultNotification"}


def command_name(comp, cmd):
    return f"{COMPONENTS.get(comp, hex(comp))}.{COMMANDS.get(comp, {}).get(cmd, hex(cmd))}"


# ---------------------------------------------------------------- log
LOG_FILE = None


def log(msg):
    line = time.strftime("%H:%M:%S ") + msg
    print(line, flush=True)
    if LOG_FILE:
        LOG_FILE.write(line + "\n")
        LOG_FILE.flush()


# ---------------------------------------------------------------- state
PUBLIC_IP = "127.0.0.1"
CONNECTING, CONNECTED = 2, 4
users = {}  # uid -> Client
games = {}  # gid -> Game
next_uid = itertools.count(1001)
HOST_NAME = ""
PLAYERS_FILE = None
invites = {}  # invitee xuid -> inviter xuid, handed out once (game invites, sent from the friends menu)
known = {}  # xuid -> {"name", "uid"}: everyone this server has seen (logins, launcher hellos), kept on disk


def load_known():
    global next_uid
    try:
        with open(PLAYERS_FILE) as f:
            for x, v in json.load(f).items():
                known[int(x, 16)] = {"name": str(v["name"])[:15], "uid": int(v["uid"])}
    except (OSError, ValueError, KeyError, TypeError):
        pass
    next_uid = itertools.count(max([1000] + [v["uid"] for v in known.values()]) + 1)


def remember(xuid, name):
    """Records a player; returns their Blaze id, the same one every session."""
    entry = known.get(xuid)
    if entry is None:
        entry = known[xuid] = {"name": name, "uid": next(next_uid)}
        log(f"** new player {name} (XUID {xuid:016X})")
    elif entry["name"] == name:
        return entry["uid"]
    entry["name"] = name
    if PLAYERS_FILE:
        try:
            with open(PLAYERS_FILE, "w") as f:
                json.dump({f"{x:016X}": v for x, v in known.items()}, f, indent=1)
        except OSError as e:
            log(f"!! cannot save players: {e}")
    return entry["uid"]
next_gid = itertools.count(1)


def ip_int(text):
    return struct.unpack(">I", socket.inet_aton(text))[0]


class Game:
    def __init__(self, gid, host, request):
        self.gid, self.host = gid, host
        self.name = request.get("GNAM", "Skate 3 LAN")
        self.settings = request.get("GSET", 0)
        self.attrs = request.get("ATTR") or Map(STRING, STRING, [])
        self.version = request.get("VSTR", "")
        # Always peer-to-peer full mesh. The game asks for 0x84 (DirtyCast failover), which its
        # network adapter runs as client/server with a DirtyCast game server: the game itself
        # becomes a mesh endpoint under the host's id and overwrites the host player's entry
        # (calls address 0 right after creation). There is no DirtyCast server on a LAN.
        self.topology = 0x82
        self.voip = request.get("VOIP", 2)
        self.capacity = max(2, min(int(request.get("PMAX", 6) or 6), 6))
        self.hnet = request.get("HNET")
        self.player_attrs = request.get("PATT")
        self.state = 1  # init
        self.xses = self.xnnc = None
        self.players = []  # Clients, in slot order
        self.uuid = f"skate3lan-{gid}-{int(time.time())}"

    def slot(self, c):
        return self.players.index(c)


def xnaddr(ip):
    """XNADDR as lan.cpp's FillXnAddr builds it: ina, inaOnline, port 1000, MAC, online id."""
    a = socket.inet_aton(ip)
    return a + a + struct.pack(">H", 1000) + b"\x00\x1d\xd8" + a[1:] + a + b"SK3LAN" + bytes(10)


def address(c):
    """The player's network address as the 360 BlazeSDK expects it (XboxClientAddress, union key 0).
    The game sends its own in createGame (HNET) / updateNetworkInfo; otherwise it is built from the
    connection's IP, which is the Xbox address lan.cpp gives the game."""
    if c.addr is None:
        c.addr = Union(0, S(XDDR=xnaddr(c.peer[0]), XUID=u64(c.xuid)))
    return c.addr


def qos(c):
    return c.qos if c.qos is not None else S(DBPS=u32(0), NATT=i32(0), UBPS=u32(0))


def user_info(c):
    return S(AID=i64(c.uid), ALOC=u32(EN_US), EXBB=b"", EXID=u64(c.xuid), ID=u32(c.uid), NAME=c.name, ONLN=i8(1),
             PID=i64(c.uid))


def extended_data(c):
    data = S(ADDR=address(c), BPS="", CTY="", DMAP=Map(UINT32, INT32, [(0x00070047, 0)]), HWFG=u32(0),
             PSLM=Array(INT32, [10]), QDAT=qos(c), UATT=u64(0))
    if c.game:
        data.append(("ULST", Array(UINT64, [4 << 48 | 1 << 32 | c.game.gid])))
    return S(DATA=data, USID=u32(c.uid))


def player_data(c, g):
    patt = g.player_attrs if c is g.players[0] and g.player_attrs else Map(STRING, STRING, [])
    return S(BLOB=b"", EXID=u64(c.xuid), GID=u32(g.gid), LOC=u32(EN_US), NAME=c.name, NQOS=qos(c), PATT=patt,
             PID=u32(c.uid), PNET=address(c), SID=u8(g.slot(c)), SLOT=i32(0), STAT=i32(c.state),
             TEAM=u16(0xFFFF), TIDX=u16(0xFFFF), TIME=i64(c.joined_us), UID=u32(c.uid))


def game_setup(g, mmid=0):
    host = S(HPID=u32(g.host.uid), HSLT=u8(0))
    data = S(ADMN=Array(UINT32, [g.host.uid]), ATTR=g.attrs, CAP=Array(UINT16, [g.capacity, 0]), GID=u32(g.gid),
             GNAM=g.name, GPVH=u64(1), GSET=u32(g.settings), GSID=u32(g.gid), GSTA=i32(g.state), GVER=i32(1),
             HNET=g.hnet or Array(UNION, [address(g.host)]), HSES=u32(g.gid), IGNO=i8(0),
             MCAP=u16(g.capacity), NQOS=S(DBPS=u32(0), NATT=i32(0), UBPS=u32(0)), NTOP=i32(g.topology),
             PGID="", PHST=host, PSAS="", QCAP=u16(0), SEED=u32(0x5EED0000 | g.gid), THST=host, UUID=g.uuid,
             VOIP=i32(g.voip), VSTR=g.version)
    if g.xnnc is not None:
        data.append(("XNNC", g.xnnc))
    if g.xses is not None:
        data.append(("XSES", g.xses))
    return S(ERR=u32(0), GAME=data, MMID=u32(mmid), PROS=Array(STRUCT, [player_data(p, g) for p in g.players]))


# ---------------------------------------------------------------- Blaze connection
class Client:
    def __init__(self, reader, writer):
        self.reader, self.writer = reader, writer
        self.peer = writer.get_extra_info("peername")
        self.uid = None
        self.name = "?"
        self.xuid = 0
        self.addr = None  # the Union the game sent in updateNetworkInfo, replayed to others as is
        self.qos = None
        self.game = None
        self.state = CONNECTING
        self.joined_us = 0

    def __str__(self):
        return f"{self.name}#{self.uid}" if self.uid else f"{self.peer[0]}:{self.peer[1]}"

    def send(self, comp, cmd, fields, mtype, info=0, error=0):
        payload = encode(fields)
        if len(payload) > 0xFFFF:
            log(f"!! {self}: {command_name(comp, cmd)} payload {len(payload)} bytes is too big, dropped")
            return
        self.writer.write(struct.pack(">HHHHI", len(payload), comp, cmd, error, mtype << 28 | info) + payload)

    def notify(self, comp, cmd, fields):
        log(f"<- {self} {NOTIFY_NAMES.get((comp, cmd), command_name(comp, cmd))}{show(fields, 2)}")
        self.send(comp, cmd, fields, 2)


async def serve_client(reader, writer):
    c = Client(reader, writer)
    log(f"++ connection from {c}")
    try:
        while True:
            head = await reader.readexactly(12)
            length, comp, cmd, error, info = struct.unpack(">HHHHI", head)
            payload = await reader.readexactly(length)
            name = command_name(comp, cmd)
            try:
                request = decode(payload)
                log(f"-> {c} {name}{show(request, 2)}")
            except ValueError as e:
                log(f"-> {c} {name}: undecodable ({e}) {payload.hex()}")
                request = Struct()
            handler = HANDLERS.get((comp, cmd))
            if handler is None:
                log(f"   (no handler for {name}: empty reply)")
            reply, notes = Struct(), []
            try:
                result = handler(c, request) if handler else None
                if isinstance(result, tuple):
                    reply, notes = result
                elif result is not None:
                    reply = result
            except Exception as e:  # keep the session alive; the log shows what broke
                log(f"!! {name} handler failed: {e!r}")
            log(f"<- {c} {name} reply{show(reply, 2)}")
            c.send(comp, cmd, reply, 1, info & 0x0FFFFFFF)
            for target, ncomp, ncmd, fields in notes:
                target.notify(ncomp, ncmd, fields)
            await writer.drain()
    except (asyncio.IncompleteReadError, ConnectionError):
        pass
    finally:
        log(f"-- {c} disconnected")
        leave_game(c)
        users.pop(c.uid, None)
        writer.close()


# ---------------------------------------------------------------- handlers
def h_server_instance(c, req):  # redirector: Blaze is this same port, plain TCP
    return S(ADDR=Union(0, S(HOST=PUBLIC_IP, IP=u32(ip_int(PUBLIC_IP)), PORT=u16(BLAZE_PORT))), SECU=i8(0),
             XDNS=u32(0))


def h_pre_auth(c, req):
    return S(CIDS=Array(UINT16, [1, 4, 7, 8, 9, 0xB, 0xC, 0xF, 0x19, 0x7800, 0x7802, 0x7803]),
             CONF=S(CONF=Map(STRING, STRING, [("pingPeriodInMs", "15000"), ("voipHeadsetUpdateRate", "1000")])),
             QOSS=S(BWPS=S(PSA=PUBLIC_IP, PSP=u16(17502), SNA="lan"), LNP=u16(10),
                    LTPS=Map(STRING, STRUCT, []), SVID=u32(1)),
             SVER="Skate 3 LAN 1.0")


def h_post_auth(c, req):
    return S(TELE=S(ADRS=PUBLIC_IP, ANON=i8(0), DISA="", FILT="", LOC=u32(EN_US), NOOK="US,CA,MX", PORT=u32(9946),
                    SDLY=u32(15000), SKEY="lan", SPCT=u32(75)),
             TICK=S(ADRS=PUBLIC_IP, PORT=u32(8999), SKEY=f"{c.uid},{PUBLIC_IP}:8999,skate-2010-xbl2,10,50,50,50,50,0,0"))


def h_ping(c, req):
    return S(STIM=u32(int(time.time())))


def login_session(c):
    now = int(time.time())
    return S(BUID=u32(c.uid), FRST=i8(0), KEY=f"lan-{c.uid}", LLOG=i64(now), MAIL="",
             PDTL=S(DSNM=c.name, LAST=u32(now), PID=i64(c.uid), XREF=u64(c.xuid), XTYP=i32(1)), UID=i64(c.uid))


def h_login(c, req):
    # xboxLogin: GTAG + XUID (the launcher's name and XUID, via lan.cpp). Other logins: any string / id.
    strings = [v for _, v in req if isinstance(v, str) and v and "@" not in v]
    xuids = [v.value for _, v in req if isinstance(v, Int) and v.type in (INT64, UINT64) and v.value]
    c.name = (req.get("GTAG") or (strings[0] if strings else "Skater"))[:15]
    c.xuid = req.get("XUID") or (xuids[0] if xuids else 0)
    if not c.xuid or any(u.xuid == c.xuid for u in users.values() if u is not c):
        # Two default recomp profiles share one XUID; the game takes a player with its own
        # XUID for itself ("local player"). Older launchers: give this one its own.
        own = 0x0009000000000000 | next(next_uid)
        log(f"!! {c.name}: XUID {c.xuid:016X} missing or already online, using {own:016X}")
        c.xuid = own
    c.uid = remember(c.xuid, c.name)
    users[c.uid] = c
    log(f"** {c} logged in (XUID {c.xuid:016X})")
    reply = S(AGUP=i8(0), PRIV="", SESS=login_session(c), SPAM=i8(1), THST="", TURI="")
    notes = [(c, 0x7802, NOTIFY_USER_ADDED, user_info(c)), (c, 0x7802, NOTIFY_EXTENDED_DATA, extended_data(c))]
    for u in users.values():  # friends already online see this one come online
        if u is not c:
            notes += [(u, 0x7802, NOTIFY_USER_ADDED, user_info(c)), (u, 0x7802, NOTIFY_EXTENDED_DATA, extended_data(c))]
    return reply, notes


def h_session_data(c, req):
    return login_session(c)


def h_network_info(c, req):
    if isinstance(req.get("ADDR"), Union):
        c.addr = req.get("ADDR")
    if isinstance(req.get("NQOS"), Struct):
        c.qos = req.get("NQOS")
    return Struct()


def h_lookup_users(c, req):
    wanted = req.get("ULST")
    found = []
    for want in (wanted.items if isinstance(wanted, Array) else []):
        if isinstance(want, Struct):
            for u in users.values():
                if (want.get("ID") == u.uid or (want.get("NAME") and want.get("NAME") == u.name)
                        or (want.get("EXID") and want.get("EXID") == u.xuid)):
                    found.append(user_info(u))
    return S(ULST=Array(STRUCT, found)) if found else Struct()


def offline_member(xuid, name, uid):
    edat = S(ADDR=Union(0, S(XDDR=bytes(36), XUID=u64(xuid))), BPS="", CTY="",
             DMAP=Map(UINT32, INT32, [(0x00070047, 0)]), HWFG=u32(0), PSLM=Array(INT32, [10]),
             QDAT=S(DBPS=u32(0), NATT=i32(0), UBPS=u32(0)), UATT=u64(0))
    user = S(AID=i64(uid), ALOC=u32(EN_US), EXBB=b"", EXID=u64(xuid), ID=u32(uid), NAME=name, ONLN=i8(0),
             PID=i64(uid))
    return S(MATS=i64(int(time.time())), MUIF=S(EDAT=edat, FLGS=u32(0), USER=user))


def h_get_lists(c, req):
    """Association lists. On a LAN everyone on this server is a friend: friendList holds every other
    player online (member = MATS time added, MUIF {EDAT extended data, FLGS, USER}); other lists empty."""
    wanted = req.get("ALST")
    names = [w.get("ALNM") for w in (wanted.items if isinstance(wanted, Array) else []) if isinstance(w, Struct)]
    lists = []
    for i, name in enumerate(names or ["friendList", "recentPlayerList"]):
        members = []
        if name == "friendList":
            members = [S(MATS=i64(int(time.time())), MUIF=S(EDAT=extended_data(u).get("DATA"), FLGS=u32(1),
                                                             USER=user_info(u)))
                       for u in users.values() if u is not c]
            # Players seen before but not online yet: the list is only sent at login (the game has
            # no "member added" notification), so they must be in it already when they log in.
            online = {u.xuid for u in users.values()}
            members += [offline_member(x, v["name"], v["uid"]) for x, v in known.items()
                        if x != c.xuid and x not in online]
        info = S(BOID=u64(0x19 << 48 | (i + 1) << 32 | c.uid), LID=u32(i + 1), LMN=name, LMS=u32(100),
                 RFLG=i8(0), SFLG=i8(0))
        if members:
            info.append(("ALML", Array(STRUCT, members)))
        lists.append((name, info))
    return S(ALMP=Map(STRING, STRUCT, lists))


def h_submit_stats(c, req):  # gamereporting 0x2: the game waits for its report to come back processed
    report = S(FNSH=i8(1), GRID=u32(0), GTYP=u32(req.get("GTYP", 1)), PRCS=i8(0),
               RPRT=req.get("RPRT") or Map(UINT32, STRUCT, []))
    return Struct(), [(c, 0xC, 0x72, S(EROR=i32(0), FNL=i8(0), GRID=u32(0), RPRT=report))]


def h_client_config(c, req):
    return S(CONF=Map(STRING, STRING, []))


# ---- game manager: one freeskate per LAN, the first player to look for one hosts it
def h_create_game(c, req):
    leave_game(c)
    hnet = req.get("HNET")
    if isinstance(hnet, Array) and hnet.items and isinstance(hnet.items[0], Union) and hnet.items[0].key != 0x7F:
        c.addr = hnet.items[0]  # the host's own address, as its game reports it
    g = Game(next(next_gid), c, req)
    games[g.gid] = g
    c.game, c.state, c.joined_us = g, CONNECTED, int(time.time() * 1e6)
    g.players.append(c)
    log(f"** {c} hosts game {g.gid} \"{g.name}\"")
    return S(GID=u32(g.gid)), [(c, 4, NOTIFY_JOIN_GAME, game_setup(g))]


def open_game():
    return next((g for g in games.values() if len(g.players) < g.capacity), None)


def join(c, g, mmid=0):
    """Notifications that put c into g (sent after the reply)."""
    notes = []
    for p in g.players:
        notes += [(c, 0x7802, NOTIFY_USER_ADDED, user_info(p)), (c, 0x7802, NOTIFY_EXTENDED_DATA, extended_data(p))]
    leave_game(c)
    c.game, c.state, c.joined_us = g, CONNECTING, int(time.time() * 1e6)
    g.players.append(c)
    log(f"** {c} joins game {g.gid}")
    for p in g.players[:-1]:
        notes += [(p, 0x7802, NOTIFY_USER_ADDED, user_info(c)), (p, 0x7802, NOTIFY_EXTENDED_DATA, extended_data(c)),
                  (p, 4, NOTIFY_PLAYER_JOINING, S(GID=u32(g.gid), PDAT=player_data(c, g)))]
    notes.append((c, 4, NOTIFY_JOIN_GAME, game_setup(g, mmid)))
    return notes


def h_start_matchmaking(c, req):
    msid = c.uid
    g = open_game()
    if g is None:  # nobody hosts yet: the game then creates one (resetDedicatedServer / createGame)
        log(f"** {c}: no game yet, they will host")
        return S(MSID=u32(msid)), [(c, 4, NOTIFY_MATCHMAKING_FINISHED,
                                    S(FIT=u32(0), GID=u32(0), MAXF=u32(0), MSID=u32(msid), RSLT=i32(3), USID=u32(c.uid)))]
    finished = (c, 4, NOTIFY_MATCHMAKING_FINISHED,
                S(FIT=u32(100), GID=u32(g.gid), MAXF=u32(100), MSID=u32(msid), RSLT=i32(2), USID=u32(c.uid)))
    notes = join(c, g, msid)
    return S(MSID=u32(msid)), [finished] + notes


def h_join_game(c, req):
    """joinGame by game id, or by a player (USER: join a friend's game), else any open game."""
    target = req.get("USER")
    friend = users.get(target.get("ID")) if isinstance(target, Struct) else None
    g = games.get(req.get("GID")) or (friend.game if friend else None) or open_game()
    if g is None or len(g.players) >= g.capacity:
        return Struct()
    log(f"** {c} joins game {g.gid}" + (f" (friend {friend})" if friend else ""))
    return S(GID=u32(g.gid)), join(c, g)


def broadcast(g, comp, cmd, fields, skip=None):
    return [(p, comp, cmd, fields) for p in g.players if p is not skip]


def h_game_session(c, req):  # finalizeGameCreation / updateGameSession: the Xbox session id / key blobs
    g = games.get(req.get("GID"))
    if g:
        g.xses, g.xnnc = req.get("XSES", g.xses), req.get("XNNC", g.xnnc)
    return Struct()


def h_game_state(c, req):
    g = games.get(req.get("GID"))
    if not g:
        return Struct()
    g.state = req.get("GSTA", g.state)
    return Struct(), broadcast(g, 4, NOTIFY_GAME_STATE_CHANGE, S(GID=u32(g.gid), GSTA=i32(g.state)))


def h_game_settings(c, req):
    g = games.get(req.get("GID"))
    if not g:
        return Struct()
    g.settings = req.get("GSET", g.settings)
    return Struct(), broadcast(g, 4, NOTIFY_GAME_SETTINGS_CHANGE, S(ATTR=u32(g.settings), GID=u32(g.gid)))


def h_game_attributes(c, req):
    g = games.get(req.get("GID"))
    attrs = req.get("ATTR")
    if not g or not isinstance(attrs, Map):
        return Struct()
    merged = dict(g.attrs.items)
    merged.update(attrs.items)
    g.attrs = Map(STRING, STRING, merged.items())
    return Struct(), broadcast(g, 4, NOTIFY_GAME_ATTRIB_CHANGE, S(ATTR=attrs, GID=u32(g.gid)))


def h_player_connection(c, req):  # updatePlayerConnection / updateMeshConnection
    g = games.get(req.get("GID"))
    targets = req.get("TARG")
    notes = []
    for t in (targets.items if g and isinstance(targets, Array) else []):
        p = users.get(t.get("PID")) if isinstance(t, Struct) else None
        if not p or p.game is not g:
            continue
        # Mesh connection status (360): 0 disconnected, 1 establishing, 2 connected.
        if t.get("STAT") == 2 and p.state != CONNECTED:
            p.state = CONNECTED
            notes += broadcast(g, 4, NOTIFY_PLAYER_JOIN_COMPLETED, S(GID=u32(g.gid), PID=u32(p.uid)))
        elif t.get("STAT") == 0:
            notes += remove_player(p, reason=1)
    return Struct(), notes


def h_remove_player(c, req):
    p = users.get(req.get("PID"), c)
    return Struct(), remove_player(p, reason=6)


def remove_player(p, reason):
    g = p.game
    if not g or p not in g.players:
        return []
    notes = broadcast(g, 4, NOTIFY_PLAYER_REMOVED, S(CNTX=u16(0), GID=u32(g.gid), PID=u32(p.uid), REAS=i32(reason)))
    g.players.remove(p)
    p.game = None
    log(f"** {p} left game {g.gid}")
    if p is g.host or not g.players:  # no host migration: the game ends with its host
        notes += broadcast(g, 4, NOTIFY_GAME_REMOVED, S(GID=u32(g.gid)))
        for other in g.players:
            other.game = None
        games.pop(g.gid, None)
        log(f"** game {g.gid} closed")
    return notes


def leave_game(c):
    for target, comp, cmd, fields in remove_player(c, reason=1):
        if target is not c:
            target.notify(comp, cmd, fields)


def h_destroy_game(c, req):
    g = games.get(req.get("GID"))
    return Struct(), (remove_player(g.host, reason=6) if g else [])


HANDLERS = {
    (5, 0x1): h_server_instance,
    (9, 0x7): h_pre_auth,
    (9, 0x8): h_post_auth,
    (9, 0x2): h_ping,
    (9, 0x1): h_client_config,
    (1, 0xAA): h_login,
    (1, 0xC8): h_login,
    (1, 0x28): h_login,
    (1, 0x3C): h_login,
    (1, 0x32): h_login,
    (1, 0x6E): h_session_data,
    (1, 0xE6): h_session_data,
    (0x7802, 0x14): h_network_info,
    (0x7802, 0xD): h_lookup_users,
    (0x19, 0x6): h_get_lists,
    (0xC, 0x2): h_submit_stats,
    (4, 0x1): h_create_game,
    (4, 0x19): h_create_game,
    (4, 0xD): h_start_matchmaking,
    (4, 0x9): h_join_game,
    (4, 0xF): h_game_session,
    (4, 0x1A): h_game_session,
    (4, 0x3): h_game_state,
    (4, 0x4): h_game_settings,
    (4, 0x7): h_game_attributes,
    (4, 0xA): h_player_connection,
    (4, 0x1D): h_player_connection,
    (4, 0xB): h_remove_player,
    (4, 0x2): h_destroy_game,
}

# ---------------------------------------------------------------- HTTP
CONFIG_XML = """<?xml version="1.0" encoding="utf-8"?>
<Config>
  <Reward>15943774168473329290</Reward>
  <GameSettings>
    <OpenJoinTime0>60</OpenJoinTime0><OpenJoinTime1>60</OpenJoinTime1><OpenJoinTime2>45</OpenJoinTime2>
    <OpenJoinTime3>45</OpenJoinTime3><OpenJoinTime4>30</OpenJoinTime4><OpenJoinTime5>30</OpenJoinTime5>
    <LockedTime>60</LockedTime><MaxGameTime>600</MaxGameTime><HurryTime>30</HurryTime>
    <ResultsTime>5</ResultsTime><RewardsTime>12</RewardsTime><ChangeTime>10</ChangeTime>
    <TimeoutMS>5000</TimeoutMS><ProposalTimeout>30</ProposalTimeout><MaxWaitForResetTime>120</MaxWaitForResetTime>
    <InactiveCountThreshold>2</InactiveCountThreshold><DeathRaceInactiveThreshold>60.0</DeathRaceInactiveThreshold>
  </GameSettings>
  <MatchmakingSettings>
    <AllowDLC>false</AllowDLC><BypassDirtyCast>false</BypassDirtyCast>
    <CreateGameDurationMS>11000</CreateGameDurationMS><FindGameDurationMS>5000</FindGameDurationMS>
    <ForceNatWarningMessage>false</ForceNatWarningMessage>
  </MatchmakingSettings>
  <AISchemaUploadsSettings>
    <OnStartCheckSchemaAndReUploadIfRequired>false</OnStartCheckSchemaAndReUploadIfRequired>
    <OnStartAlwaysUpdateAIProfile>false</OnStartAlwaysUpdateAIProfile>
    <UpdateUploadedAIProfileEveryNTicks>false</UpdateUploadedAIProfileEveryNTicks>
    <UpdateUploadedAIProfileEveryNTicks_NumTicks>216000</UpdateUploadedAIProfileEveryNTicks_NumTicks>
  </AISchemaUploadsSettings>
  <FESettings>
    <EnableOnlineTipsLoaderData>false</EnableOnlineTipsLoaderData>
    <EnableFriendsLeaderboardData>false</EnableFriendsLeaderboardData>
    <TipsDataNumPagesWaitForOnlineData>2</TipsDataNumPagesWaitForOnlineData>
    <FriendsLeaderboardDataDelayMS>2000</FriendsLeaderboardDataDelayMS>
  </FESettings>
</Config>
"""


# Replies of Skateboard3Server's SkateProfile service; anything else gets "true".
WEB_REPLIES = {
    "StartLoginProcess": "<LoginInfoContainer><privacyFlagContainer /><awardedBoardSales>0</awardedBoardSales>"
                         "<teamInfo><teamId>0</teamId><numMembers>0</numMembers></teamInfo></LoginInfoContainer>",
    "UploadSchema": "<LongContainer><value>1</value></LongContainer>",
    "UploadAIProfile": "<LongContainer><value>2</value></LongContainer>",
    "SetUserDLC": "<BoolContainer><value>true</value></BoolContainer>",
    "SetUserAchievements": "<IntegerContainer><value>0</value></IntegerContainer>",
}


def query(path):
    return dict(kv.partition("=")[::2] for kv in path.partition("?")[2].split("&") if kv)


def friends_text(path):
    """The Xbox friend list lan.cpp gives the game's friends menu: everyone known except the asker,
    one per line: "<xuid hex> <online 0/1> <in a game 0/1> <name>", then "INVITE <xuid hex> <ip>"
    once if someone invited the asker."""
    me = query(path).get("me", "")[:16]
    online = {u.xuid: u for u in list(users.values())}
    lines = []
    try:
        inviter = invites.pop(int(me, 16), None)
    except ValueError:
        inviter = None
    if inviter is not None and inviter in online:
        lines.append(f"INVITE {inviter:016X} {online[inviter].peer[0]}")
    for xuid, v in sorted(list(known.items()), key=lambda kv: kv[1]["name"].lower()):
        if f"{xuid:016X}" == me.upper():
            continue
        u = online.get(xuid)
        lines.append(f"{xuid:016X} {int(u is not None)} {int(bool(u and u.game))} {v['name']}")
    return "\n".join(lines) + "\n"


class HttpHandler(http.server.BaseHTTPRequestHandler):
    # HTTP/1.1 so "Expect: 100-continue" gets its 100 at once: the game's web service
    # POSTs send it and otherwise wait ~10 s each ("Updating Reports" took half a minute).
    protocol_version = "HTTP/1.1"
    def respond(self, code, data=b"", kind="text/xml"):
        # One request per connection, as with HTTP/1.0: the game's HTTP client reads until
        # the server closes (a kept-alive connection left it waiting, then hit a closed socket).
        self.send_response(code)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)
        self.close_connection = True

    def do_GET(self):
        if self.path.startswith("/skate3/config/") and self.path.endswith(".xml"):
            log(f"http {self.client_address[0]} GET {self.path} -> config")
            return self.respond(200, CONFIG_XML.encode())
        if self.path.startswith("/skate3lan/invite"):
            q = query(self.path)
            try:
                frm, to = int(q.get("from", ""), 16), int(q.get("to", ""), 16)
            except ValueError:
                return self.respond(400)
            invites[to] = frm
            log(f"** invite: {known.get(frm, {}).get('name', frm)} -> {known.get(to, {}).get('name', to)}")
            return self.respond(200, b"ok\n", "text/plain")
        if self.path.startswith("/skate3lan/friends"):
            return self.respond(200, friends_text(self.path).encode(), "text/plain")
        log(f"http {self.client_address[0]} GET {self.path} -> 404")
        self.respond(404)

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        data = self.rfile.read(length)
        if self.path.startswith("/skate3/ws/"):  # SOAP-ish web services (SkateProfile.asmx/Method): plain XML replies
            method = self.path.rsplit("/", 1)[-1].split("?")[0]
            body = WEB_REPLIES.get(method, "<BoolContainer><value>true</value></BoolContainer>")
            log(f"http {self.client_address[0]} POST {self.path} ({length} bytes) -> {body[:60]}")
            return self.respond(200, ('<?xml version="1.0" encoding="utf-8"?>' + body).encode())
        log(f"http {self.client_address[0]} POST {self.path} ({length} bytes) -> 404: {data[:200]!r}")
        self.respond(404)

    def log_message(self, *args):
        pass


# ---------------------------------------------------------------- main
def lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))  # no packet is sent; picks the LAN interface
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


async def sink(reader, writer):
    """Telemetry (9946) and ticker (8999): accepted and ignored."""
    try:
        while await reader.read(4096):
            pass
    except ConnectionError:
        pass
    writer.close()


class Discovery(asyncio.DatagramProtocol):
    """Hellos from launchers ("SKATE3LAN-HELLO <xuid hex> <name>"): those players join the friend list."""

    def datagram_received(self, data, addr):
        parts = data.decode("utf-8", "replace").split(" ", 2)
        if len(parts) == 3 and parts[0] == "SKATE3LAN-HELLO":
            try:
                remember(int(parts[1], 16), parts[2].strip()[:15] or "Skater")
            except ValueError:
                pass


async def beacon():
    """Tells launchers on the LAN where the server is, every second."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.setblocking(False)
    message = f"SKATE3LAN 1 {PUBLIC_IP} {HOST_NAME or 'Host'}".encode()
    while True:
        for target in ("255.255.255.255", PUBLIC_IP.rsplit(".", 1)[0] + ".255"):
            try:
                s.sendto(message, (target, BEACON_PORT))
            except OSError:
                pass
        await asyncio.sleep(1)


async def main_async():
    server = await asyncio.start_server(serve_client, "0.0.0.0", BLAZE_PORT)
    for port in (9946, 8999):
        try:
            await asyncio.start_server(sink, "0.0.0.0", port)
        except OSError as e:
            log(f"!! port {port} unavailable ({e})")
    log(f"Skate 3 LAN server: Blaze on {PUBLIC_IP}:{BLAZE_PORT}")
    try:
        httpd = http.server.ThreadingHTTPServer(("0.0.0.0", HTTP_PORT), HttpHandler)
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        log(f"Skate 3 LAN server: HTTP on {PUBLIC_IP}:{HTTP_PORT}")
    except OSError as e:
        log(f"!! HTTP port {HTTP_PORT} unavailable ({e}); the game's online config will be missing")
    loop = asyncio.get_running_loop()
    try:
        await loop.create_datagram_endpoint(Discovery, local_addr=("0.0.0.0", HELLO_PORT))
    except OSError as e:
        log(f"!! discovery port {HELLO_PORT} unavailable ({e})")
    asyncio.ensure_future(beacon())
    log(f"Friends' launchers find this server by itself (or enter {PUBLIC_IP}).")
    async with server:
        await server.serve_forever()


def self_test():
    # Byte vectors from Skateboard3Server's serializer tests (BlazeSerializerFacts).
    cases = [
        (S(TSTA="testing", TSTB=i8(1), TSTC=u8(0x80), TSTD=Int(INT16, 1234), TSTE=u16(1234), TSTF=i32(1234),
           TSTG=u32(1234), TSTH=i64(1234), TSTI=u64(1234), TSTJ=u16(2)),
         "d33d2118" + b"testing\0".hex() + "d33d222101d33d233180d33d244204d2d33d255204d2d33d2664000004d2"
         "d33d2774000004d2d33d288800000000000004d2d33d299800000000000004d2d33d2a520002"),
        (S(TSTA=b"\xde\xad\xbe\xef"), "d33d21b4deadbeef"),
        (S(TSTA=Array(STRING, ["Test1", "areallylongstring", "testing"])),
         "d33d21a10316" + b"Test1\0".hex() + "12" + b"areallylongstring\0".hex() + "08" + b"testing\0".hex()),
        (S(TSTA=Array(INT32, [1234, 10000])), "d33d21a10264000004d200002710"),
        (S(TSTA=Map(INT32, STRING, [(1, "Test1"), (2, "areallylongstring"), (3, "testing")])),
         "d33d21c36f04000000011f06" + b"Test1\0".hex() + "0000000212" + b"areallylongstring\0".hex()
         + "0000000308" + b"testing\0".hex()),
        (S(TSTA=Map(STRING, STRUCT, [("a", S(TSTA="testing1")), ("b", S(TSTA="testing2"))])),
         "d33d21c21f026100" + "00d33d2119" + b"testing1\0".hex() + "00" + "026200" + "d33d2119"
         + b"testing2\0".hex() + "00"),
        (S(TSTA=S(TSTA=S(TSTA="testing"))), "d33d2100d33d2100d33d2118" + b"testing\0".hex() + "0000"),
        (Struct(), ""),
    ]
    for fields, want in cases:
        got = encode(fields).hex()
        assert got == want, f"encode {show(fields)}\n got  {got}\n want {want}"
        assert encode(decode(bytes.fromhex(want))).hex() == want, f"round trip {want}"
    # A whole game setup survives decode -> encode, unions and arrays of structs included.
    c = Client.__new__(Client)
    c.uid, c.name, c.xuid, c.qos, c.state, c.joined_us, c.game = 1001, "host", 9, None, CONNECTED, 5, None
    c.addr = None
    c.peer = ("192.168.1.2", 1)
    g = Game(1, c, S(GNAM="game", ATTR=Map(STRING, STRING, [("k", "v")])))
    g.players.append(c)
    c.game = g
    blob = encode(game_setup(g))
    assert encode(decode(blob)) == blob
    pnet = decode(blob).get("PROS").items[0].get("PNET")
    assert pnet.key == 0 and pnet.value.get("XDDR")[:4] == bytes([192, 168, 1, 2]) and len(pnet.value.get("XDDR")) == 36
    assert encode(extended_data(c)) == encode(decode(encode(extended_data(c))))
    known.clear()
    known.update({0x0009000000000042: {"name": "friend", "uid": 1002}, 9: {"name": "host", "uid": 1001}})
    assert friends_text("/skate3lan/friends?me=0000000000000009") == "0009000000000042 0 0 friend\n"
    off = offline_member(0x0009000000000042, "friend", 1002)
    assert encode(decode(encode(off))) == encode(off)
    print("self-test ok")


def main():
    global PUBLIC_IP, LOG_FILE, HOST_NAME, PLAYERS_FILE
    ap = argparse.ArgumentParser(description="Skate 3 LAN server")
    ap.add_argument("--ip", help="the IP players connect to (default: this Mac's LAN IP)")
    ap.add_argument("--log", help="also append the log to this file")
    ap.add_argument("--name", default="", help="the host's name, shown in friends' launchers")
    ap.add_argument("--players", help="remembers every player seen in this JSON file (friend list)")
    ap.add_argument("--test", action="store_true", help="run the codec self-check and exit")
    args = ap.parse_args()
    if args.test:
        return self_test()
    PUBLIC_IP = args.ip or lan_ip()
    if args.log:
        LOG_FILE = open(args.log, "a")
    HOST_NAME, PLAYERS_FILE = args.name[:15], args.players
    if PLAYERS_FILE:
        load_known()
    try:
        asyncio.run(main_async())
    except OSError as e:
        log(f"!! cannot listen on port {BLAZE_PORT}: {e}")
        sys.exit(1)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
