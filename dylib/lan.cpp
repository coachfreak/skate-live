// Skate 3 LAN, injected into the official skate3recomp app (v2.0.2, game TU3)
// with DYLD_INSERT_LIBRARIES; the Skate 3 LAN launcher does that.
//
// The recomp ships the Xbox network layer switched off (no link, no DNS, a
// loopback Xbox address). This switches it on for a local network and points the
// game's EA online code (BlazeSDK + DirtySock) at a Skate 3 LAN server
// (server/skate3_lan_server.py) instead of EA's dead servers:
//  - Xbox Live: signed in with the recomp profile's gamertag / XUID, multiplayer allowed.
//  - XNet: link up, this Mac's LAN IP as its Xbox address, other players' Xbox
//    addresses map straight to their IPs (no Xbox Live security layer, no keys).
//  - DNS: every *.ea.com name resolves to the server.
//  - Sockets: Winsock error codes and the select / getsockname / getsockopt
//    behaviour DirtySock expects (the runtime's are POSIX-shaped or stubs).
//  - BlazeHub::initialize: plain TCP to the server instead of SSL to EA.
//
// Every interposed export keeps the guest calling convention: arguments in
// r3..r10 (then the stack), result in r3, guest memory big-endian.

#include <arpa/inet.h>
#include <dlfcn.h>
#include <ifaddrs.h>
#include <libkern/OSCacheControl.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <fcntl.h>
#include <unistd.h>

#include <mach/mach.h>
#include <mach/mach_vm.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <tuple>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <rex/kernel/xam/input_injection.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xmemory.h>
#include <rex/system/xevent.h>
// xsocket.h names enum members after the BSD socket macros.
#pragma push_macro("AF_INET")
#pragma push_macro("SOCK_STREAM")
#pragma push_macro("SOCK_DGRAM")
#pragma push_macro("IPPROTO_TCP")
#pragma push_macro("IPPROTO_UDP")
#undef AF_INET
#undef SOCK_STREAM
#undef SOCK_DGRAM
#undef IPPROTO_TCP
#undef IPPROTO_UDP
#include <rex/system/xsocket.h>
#pragma pop_macro("AF_INET")
#pragma pop_macro("SOCK_STREAM")
#pragma pop_macro("SOCK_DGRAM")
#pragma pop_macro("IPPROTO_TCP")
#pragma pop_macro("IPPROTO_UDP")
#include <rex/system/xthread.h>

#include "title_logo.h"

namespace {

constexpr uint16_t kBlazePort = 42127;  // the server's only Blaze port (redirector and Blaze)

uint32_t g_server = 0x7F000001;  // IPv4, host order
uint32_t g_local = 0x7F000001;   // this Mac's LAN IP, host order: our Xbox address
char g_server_text[16] = "127.0.0.1";
// The player's name and XUID online (launcher: SKATE3_LAN_NAME / SKATE3_LAN_XUID).
// Default recomp profiles all share one XUID, which ConnApi uses as the machine address.
std::string g_name;
uint64_t g_xuid = 0;

// ---------------------------------------------------------------- log
std::mutex g_log_mutex;
FILE* g_log = nullptr;

void Log(const char* fmt, ...) {
  std::lock_guard lock(g_log_mutex);
  if (!g_log) return;
  va_list args;
  va_start(args, fmt);
  std::vfprintf(g_log, fmt, args);
  va_end(args);
  std::fputc('\n', g_log);
  if (std::strncmp(fmt, "[blaze]", 7) != 0) std::fflush(g_log);  // BlazeSDK's chatter stays buffered
}

// Peer traffic per second (lan.log "net:" lines while there is any).
std::atomic<uint32_t> g_sent_packets{0}, g_sent_bytes{0}, g_recv_packets{0}, g_recv_bytes{0}, g_send_errors{0};

// Last network call per guest thread, for the lock watchdog.
std::mutex g_track_mutex;
std::map<uint32_t, std::string> g_last_call;
void Track(const char* what) {
  const uint32_t tid = rex::system::XThread::GetCurrentThreadId();
  std::lock_guard lock(g_track_mutex);
  g_last_call[tid] = what;
}

std::string IpText(uint32_t ip) {
  char s[16];
  std::snprintf(s, sizeof(s), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
  return s;
}

// ---------------------------------------------------------------- guest memory
uint8_t* G(uint8_t* base, uint32_t addr) {
  return base + addr + (addr >= 0xE0000000u ? 0x1000u : 0u);  // physical heap host offset (macOS arm64)
}
uint32_t Get32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return std::byteswap(v);
}
uint16_t Get16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
void Put32(uint8_t* p, uint32_t v) {
  v = std::byteswap(v);
  std::memcpy(p, &v, 4);
}
void Put16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}
// Argument `i` (0-based) of a guest call; past r10 they sit in 8-byte stack slots.
uint32_t Arg(const PPCContext& ctx, uint8_t* base, int i) {
  const PPCRegister* regs[8] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
  if (i < 8) return regs[i]->u32;
  return Get32(G(base, ctx.r1.u32 + 0x54 + (i - 8) * 8));
}
void Return(PPCContext& ctx, uint32_t v) { ctx.r3.u64 = v; }

uint32_t Alloc(uint32_t size) { return rex::Runtime::instance()->memory()->SystemHeapAlloc(size); }

void SetEvent(uint32_t handle) {
  if (!handle) return;
  if (auto event = rex::system::kernel_state()->object_table()->LookupObject<rex::system::XEvent>(handle)) {
    event->Set(0, false);
  }
}

int NativeSocket(uint32_t handle) {
  auto socket = rex::system::kernel_state()->object_table()->LookupObject<rex::system::XSocket>(handle);
  return socket ? int(socket->native_handle()) : -1;
}

// ---------------------------------------------------------------- Winsock errors
uint32_t WsaError(int e) {
  switch (e) {
    case EWOULDBLOCK: return 10035;  // WSAEWOULDBLOCK (also a non-blocking connect in progress)
    case EINPROGRESS: return 10035;
    case EALREADY: return 10037;
    case ENOTSOCK: return 10038;
    case EMSGSIZE: return 10040;
    case EADDRINUSE: return 10048;
    case EADDRNOTAVAIL: return 10049;
    case ENETDOWN: return 10050;
    case ENETUNREACH: return 10051;
    case ECONNABORTED: return 10053;
    case ECONNRESET: return 10054;
    case ENOBUFS: return 10055;
    case EISCONN: return 10056;
    case ENOTCONN: return 10057;
    case ETIMEDOUT: return 10060;
    case ECONNREFUSED: return 10061;
    case EHOSTUNREACH: return 10065;
    case EINVAL: return 10022;
    default: return 10000 + uint32_t(e);
  }
}

// After a runtime socket call: on failure, replace its last error with the
// Winsock code for errno (it leaves it stale or a DOS code DirtySock can't read).
void FixError(PPCContext& ctx) {
  const int e = errno;
  if (int32_t(ctx.r3.u32) == -1) rex::system::XThread::SetLastError(WsaError(e));
}

// ---------------------------------------------------------------- Xbox addresses
// XNADDR: ina, inaOnline (network order), wPortOnline, abEnet[6], abOnline[20].
void FillXnAddr(uint8_t* x, uint32_t ip) {
  std::memset(x, 0, 36);
  Put32(x, ip);
  Put32(x + 4, ip);
  Put16(x + 8, 1000);
  const uint8_t mac[6] = {0x00, 0x1D, 0xD8, uint8_t(ip >> 16), uint8_t(ip >> 8), uint8_t(ip)};
  std::memcpy(x + 10, mac, 6);
  Put32(x + 16, ip);
  std::memcpy(x + 20, "SK3LAN", 6);
}

uint32_t XnAddrIp(const uint8_t* x) { return Get32(x) ? Get32(x) : Get32(x + 4); }

// Logs `what` with an address (or a number) the first time that pair comes up.
void LogOnce(const char* what, uint32_t value, bool address = true) {
  static std::mutex m;
  static std::set<std::pair<const char*, uint32_t>> seen;
  std::lock_guard lock(m);
  if (!seen.insert({what, value}).second) return;
  if (address) Log("%s %s", what, IpText(value).c_str());
  else Log("%s %u", what, value);
}

// ---------------------------------------------------------------- DNS
// Every name the game looks up is one of EA's (or an internal one like
// "skate3_web"): all of them go to the server. A real lookup would hand the game
// whatever the ISP's DNS makes of them (e.g. a search page for "skate3_web").
uint32_t Resolve(const char* host) {
  in_addr a{};
  if (inet_pton(AF_INET, host, &a) == 1) return ntohl(a.s_addr);
  return g_server;
}

// ---------------------------------------------------------------- originals
#define ORIGINAL(name) extern "C" void Original_##name(PPCContext&, uint8_t*) __asm__("___imp__" #name);
ORIGINAL(XamUserGetSigninState)
ORIGINAL(XamUserGetSigninInfo)
ORIGINAL(XamUserCheckPrivilege)
ORIGINAL(XamUserGetName)
ORIGINAL(XamUserAreUsersFriends)
ORIGINAL(XamShowGameInviteUI)
ORIGINAL(NetDll_XNetStartup)
ORIGINAL(NetDll_XNetGetEthernetLinkStatus)
ORIGINAL(NetDll_XNetGetTitleXnAddr)
ORIGINAL(NetDll_XNetXnAddrToInAddr)
ORIGINAL(NetDll_XNetInAddrToXnAddr)
ORIGINAL(NetDll_XNetServerToInAddr)
ORIGINAL(NetDll_XNetConnect)
ORIGINAL(NetDll_XNetGetConnectStatus)
ORIGINAL(NetDll_XNetCreateKey)
ORIGINAL(NetDll_XNetRegisterKey)
ORIGINAL(NetDll_XNetUnregisterKey)
ORIGINAL(NetDll_XNetQosLookup)
ORIGINAL(NetDll_XNetQosListen)
ORIGINAL(NetDll_XNetDnsLookup)
ORIGINAL(NetDll_connect)
ORIGINAL(NetDll_bind)
ORIGINAL(NetDll_accept)
ORIGINAL(NetDll_recv)
ORIGINAL(NetDll_recvfrom)
ORIGINAL(NetDll_send)
ORIGINAL(NetDll_sendto)
ORIGINAL(NetDll_select)
ORIGINAL(NetDll_ioctlsocket)
ORIGINAL(NetDll_setsockopt)
ORIGINAL(NetDll_getsockopt)
ORIGINAL(NetDll_getsockname)
ORIGINAL(NetDll_getpeername)
ORIGINAL(NetDll_closesocket)
ORIGINAL(XamLoaderTerminateTitle)
ORIGINAL(NetDll_WSARecvFrom)
ORIGINAL(NetDll_WSARecv)
ORIGINAL(NetDll_WSAGetOverlappedResult)
ORIGINAL(XMsgStartIORequest)
ORIGINAL(XMsgStartIORequestEx)
ORIGINAL(XMsgInProcessCall)
ORIGINAL(XamLoaderLaunchTitle)

// ---------------------------------------------------------------- Xbox Live
void SignInToLive() {
  static std::once_flag once;
  std::call_once(once, [] {
    // The recomp profile has its own "signed in to Live" switch; XUID lookups
    // for online users only answer when it is on.
    rex::cvar::SetFlagByName("user_live_signed_in", "true");
    Log("Xbox Live: signed in");
  });
}

void SigninState(PPCContext& ctx, uint8_t* base) {
  Original_XamUserGetSigninState(ctx, base);
  if (ctx.r3.u32 == 1) {  // signed in locally
    SignInToLive();
    Return(ctx, 2);
  }
}

void SigninInfo(PPCContext& ctx, uint8_t* base) {
  const uint32_t user = ctx.r3.u32, info = ctx.r5.u32;
  Original_XamUserGetSigninInfo(ctx, base);
  if (ctx.r3.u32 != 0 || !info) return;
  uint8_t* p = G(base, info);  // xuid, unk, signin_state, unk, unk, name[16]
  if (Get32(p + 12) == 1) Put32(p + 12, 2);
  if (user == 0 && !g_name.empty()) std::snprintf(reinterpret_cast<char*>(p + 24), 16, "%s", g_name.c_str());
}

// (user, xuids, count, out*, overlapped): everyone on the LAN server is a friend.
void AreFriends(PPCContext& ctx, uint8_t* base) {
  const uint32_t out = ctx.r6.u32, overlapped = ctx.r7.u32;
  if (ctx.r3.u32 != 0) return Original_XamUserAreUsersFriends(ctx, base);
  if (out) {
    Put32(G(base, out), 1);
    return Return(ctx, 0);
  }
  if (overlapped) {
    rex::system::kernel_state()->CompleteOverlappedImmediateEx(overlapped, 0, 0, 1);
    return Return(ctx, 997);  // ERROR_IO_PENDING
  }
  Original_XamUserAreUsersFriends(ctx, base);
}

void UserName(PPCContext& ctx, uint8_t* base) {  // (user, buffer, length): the player's name everywhere in game
  const uint32_t user = ctx.r3.u32, buf = ctx.r4.u32, len = ctx.r5.u32;
  Original_XamUserGetName(ctx, base);
  if (ctx.r3.u32 == 0 && user == 0 && buf && len > 1 && !g_name.empty()) {
    std::snprintf(reinterpret_cast<char*>(G(base, buf)), std::min<uint32_t>(len, 16), "%s", g_name.c_str());
  }
}

void CheckPrivilege(PPCContext& ctx, uint8_t* base) {
  const uint32_t mask = ctx.r4.u32, out = ctx.r5.u32;
  Original_XamUserCheckPrivilege(ctx, base);
  if (ctx.r3.u32 == 0 && out) {
    Put32(G(base, out), 1);  // multiplayer, communications, profile viewing...: all allowed
    LogOnce("Xbox Live privilege allowed:", mask, false);
  }
}

// ---------------------------------------------------------------- XNet
void XNetStartup(PPCContext& ctx, uint8_t* base) {
  SignInToLive();
  Original_NetDll_XNetStartup(ctx, base);
  Log("XNetStartup -> %d; Xbox address %s, server %s", int(ctx.r3.u32), IpText(g_local).c_str(), g_server_text);
}

void LinkStatus(PPCContext& ctx, uint8_t*) { Return(ctx, 0x01 | 0x02 | 0x08); }  // active, 100 Mbps, full duplex

void TitleXnAddr(PPCContext& ctx, uint8_t* base) {
  if (ctx.r4.u32) FillXnAddr(G(base, ctx.r4.u32), g_local);
  Return(ctx, 0x08 | 0x20 | 0x40 | 0x80);  // DHCP, gateway, DNS, online
}

void XnAddrToInAddr(PPCContext& ctx, uint8_t* base) {  // (caller, XNADDR*, XNKID*, IN_ADDR*)
  const uint32_t ip = ctx.r4.u32 ? XnAddrIp(G(base, ctx.r4.u32)) : 0;
  if (ctx.r6.u32) Put32(G(base, ctx.r6.u32), ip);
  LogOnce("peer Xbox address ->", ip);
  Return(ctx, 0);
}

void InAddrToXnAddr(PPCContext& ctx, uint8_t* base) {  // (caller, IN_ADDR, XNADDR*, XNKID*)
  if (ctx.r5.u32) FillXnAddr(G(base, ctx.r5.u32), ctx.r4.u32);
  if (ctx.r6.u32) std::memset(G(base, ctx.r6.u32), 0, 8);
  Return(ctx, 0);
}

void ServerToInAddr(PPCContext& ctx, uint8_t* base) {  // (caller, IN_ADDR, service id, IN_ADDR*)
  if (ctx.r6.u32) Put32(G(base, ctx.r6.u32), ctx.r4.u32);
  LogOnce("server address ->", ctx.r4.u32);
  Return(ctx, 0);
}

void Succeed(PPCContext& ctx, uint8_t*) { Return(ctx, 0); }
void ConnectStatus(PPCContext& ctx, uint8_t*) { Return(ctx, 2); }  // XNET_CONNECT_STATUS_CONNECTED

void CreateKey(PPCContext& ctx, uint8_t* base) {  // (caller, XNKID*, XNKEY*)
  static std::mt19937 rng{std::random_device{}()};
  for (uint32_t p : {ctx.r4.u32, ctx.r5.u32}) {
    if (!p) continue;
    uint8_t* b = G(base, p);
    for (int i = 0; i < (p == ctx.r4.u32 ? 8 : 16); ++i) b[i] = uint8_t(rng());
  }
  Return(ctx, 0);
}

// (caller, cxna, apxna, apxnkid, apxnkey, cina, aina, aServiceId, cProbes, bps, flags, event, XNQOS**):
// every target answers at once with a LAN round trip.
void QosLookup(PPCContext& ctx, uint8_t* base) {
  const uint32_t count = Arg(ctx, base, 1) + Arg(ctx, base, 5);
  const uint32_t event = Arg(ctx, base, 11), out = Arg(ctx, base, 12);
  const uint32_t qos = Alloc(8 + 24 * std::max(count, 1u));
  if (!qos || !out) return Return(ctx, 0x8007000E);  // E_OUTOFMEMORY
  uint8_t* q = G(base, qos);
  std::memset(q, 0, 8 + 24 * std::max(count, 1u));
  Put32(q, count);
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t* info = q + 8 + 24 * i;
    info[0] = 0x01 | 0x02;  // complete, target contacted
    Put16(info + 2, 4);     // probes sent / received
    Put16(info + 4, 4);
    Put16(info + 12, 1);  // RTT min / median, ms
    Put16(info + 14, 2);
    Put32(info + 16, 100000000);  // up / down bits per second
    Put32(info + 20, 100000000);
  }
  Put32(G(base, out), qos);
  SetEvent(event);
  Return(ctx, 0);
}

// (caller, host, event, XNDNS**): XNDNS = status, cina, aina[8].
void DnsLookup(PPCContext& ctx, uint8_t* base) {
  const char* host = ctx.r4.u32 ? reinterpret_cast<const char*>(G(base, ctx.r4.u32)) : "";
  const uint32_t event = ctx.r5.u32, out = ctx.r6.u32;
  const uint32_t ip = Resolve(host);
  Log("DNS %s -> %s", host, ip ? IpText(ip).c_str() : "not found");
  const uint32_t dns = Alloc(40);
  if (!dns || !out) return Return(ctx, 0x8007000E);
  uint8_t* d = G(base, dns);
  std::memset(d, 0, 40);
  Put32(d, ip ? 0 : 11001);  // WSAHOST_NOT_FOUND
  Put32(d + 4, ip ? 1 : 0);
  Put32(d + 8, ip);
  Put32(G(base, out), dns);
  SetEvent(event);
  Return(ctx, 0);
}

// ---------------------------------------------------------------- sockets
std::string SockaddrText(uint8_t* base, uint32_t addr) {
  if (!addr) return "?";
  const uint8_t* a = G(base, addr);
  return IpText(Get32(a + 4)) + ":" + std::to_string(Get16(a + 2));
}

void Connect(PPCContext& ctx, uint8_t* base) {  // (caller, s, sockaddr*, len)
  Track("connect");
  const std::string to = SockaddrText(base, ctx.r5.u32);
  Original_NetDll_connect(ctx, base);
  const int e = errno;
  FixError(ctx);
  Log("connect %s -> %s", to.c_str(), int32_t(ctx.r3.u32) == 0 ? "ok" : (e == EINPROGRESS ? "in progress" : std::strerror(e)));
}

// SKATE3_LAN_IP set (several games on one Mac, each with its own address, e.g. the
// LAN IP and 127.0.0.1): binds to "any" go to that address instead, so each game
// gets its own port 10000.
bool g_bind_local = false;

void Bind(PPCContext& ctx, uint8_t* base) {
  if (g_bind_local && ctx.r5.u32 && Get32(G(base, ctx.r5.u32) + 4) == 0) Put32(G(base, ctx.r5.u32) + 4, g_local);
  const std::string at = SockaddrText(base, ctx.r5.u32);
  Original_NetDll_bind(ctx, base);
  const int e = errno;
  FixError(ctx);
  Log("bind %s -> %s", at.c_str(), int32_t(ctx.r3.u32) == 0 ? "ok" : std::strerror(e));
}

void Accept(PPCContext& ctx, uint8_t* base) { Original_NetDll_accept(ctx, base); FixError(ctx); }
void Recv(PPCContext& ctx, uint8_t* base) { Original_NetDll_recv(ctx, base); FixError(ctx); }
void RecvFrom(PPCContext& ctx, uint8_t* base) { Original_NetDll_recvfrom(ctx, base); FixError(ctx); }
void Send(PPCContext& ctx, uint8_t* base) { Original_NetDll_send(ctx, base); FixError(ctx); }
// (caller, s, buf, len, flags, to*, tolen). The runtime's sendto copies the guest's
// big-endian port and address into the host sockaddr byte-swapped: every UDP packet
// to a peer went to the wrong place (127.0.0.1:10000 -> 1.0.0.127:4135).
void SendTo(PPCContext& ctx, uint8_t* base) {
  Track("sendto");
  const uint32_t to = ctx.r8.u32;
  if (!to) {
    Original_NetDll_sendto(ctx, base);
    return FixError(ctx);
  }
  const int fd = NativeSocket(ctx.r4.u32);
  if (fd < 0) {
    rex::system::XThread::SetLastError(10038);
    return Return(ctx, uint32_t(-1));
  }
  const uint8_t* a = G(base, to);
  sockaddr_in dest{};
  dest.sin_len = sizeof(dest);
  dest.sin_family = AF_INET;
  dest.sin_port = htons(Get16(a + 2));
  dest.sin_addr.s_addr = htonl(Get32(a + 4));
  const ssize_t n = sendto(fd, G(base, ctx.r5.u32), ctx.r6.u32, 0, (sockaddr*)&dest, sizeof(dest));
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 10) {
    Log("sendto %s, %u bytes -> %d", SockaddrText(base, to).c_str(), ctx.r6.u32, int(n));
  }
  if (n < 0) {
    g_send_errors++;
    rex::system::XThread::SetLastError(WsaError(errno));
    return Return(ctx, uint32_t(-1));
  }
  g_sent_packets++;
  g_sent_bytes += uint32_t(n);
  Return(ctx, uint32_t(n));
}

// Winsock ignores nfds and the guest passes 0; POSIX select needs the highest fd + 1.
void Select(PPCContext& ctx, uint8_t* base) {
  ctx.r4.u64 = FD_SETSIZE;
  Original_NetDll_select(ctx, base);
  FixError(ctx);
}

// FIONBIO's argument is nonzero either way round; FIONREAD's result comes back host-endian.
void IoctlSocket(PPCContext& ctx, uint8_t* base) {
  const uint32_t cmd = ctx.r5.u32, arg = ctx.r6.u32;
  Original_NetDll_ioctlsocket(ctx, base);
  if (ctx.r3.u32 == 0 && cmd == 0x4004667F && arg) {
    uint8_t* p = G(base, arg);
    uint32_t v;
    std::memcpy(&v, p, 4);
    Put32(p, v);
  }
}

// 4-byte options are guest big-endian; the runtime hands them to the host as is.
void SetSockOpt(PPCContext& ctx, uint8_t* base) {  // (caller, s, level, name, value*, len)
  uint8_t* value = ctx.r7.u32 ? G(base, ctx.r7.u32) : nullptr;
  const bool swap = value && ctx.r8.u32 == 4;
  uint32_t guest = 0;
  if (swap) {
    std::memcpy(&guest, value, 4);
    const uint32_t host = std::byteswap(guest);
    std::memcpy(value, &host, 4);
  }
  Original_NetDll_setsockopt(ctx, base);
  if (swap) std::memcpy(value, &guest, 4);
  if (int32_t(ctx.r3.u32) == -1) Return(ctx, 0);  // e.g. buffer sizes the host refuses: not worth failing over
}

void GetSockOpt(PPCContext& ctx, uint8_t* base) {  // (caller, s, level, name, value*, len*)
  const int fd = NativeSocket(ctx.r4.u32);
  int value = 0;
  socklen_t len = sizeof(value);
  if (fd < 0 || !ctx.r7.u32 || getsockopt(fd, int(ctx.r5.u32), int(ctx.r6.u32), &value, &len) != 0) {
    rex::system::XThread::SetLastError(fd < 0 ? 10038 : WsaError(errno));
    return Return(ctx, uint32_t(-1));
  }
  if (ctx.r5.u32 == 0xFFFF && ctx.r6.u32 == 0x1007 && value) value = int(WsaError(value));  // SO_ERROR
  Put32(G(base, ctx.r7.u32), uint32_t(value));
  if (ctx.r8.u32) Put32(G(base, ctx.r8.u32), 4);
  Return(ctx, 0);
}

// (caller, s, sockaddr*, len*) into the guest's sockaddr_in: family, port, address, all big-endian.
void SockName(PPCContext& ctx, uint8_t* base, bool peer) {
  const int fd = NativeSocket(ctx.r4.u32);
  sockaddr_in a{};
  socklen_t len = sizeof(a);
  if (fd < 0 || !ctx.r5.u32 || (peer ? getpeername(fd, (sockaddr*)&a, &len) : getsockname(fd, (sockaddr*)&a, &len)) != 0) {
    rex::system::XThread::SetLastError(fd < 0 ? 10038 : WsaError(errno));
    return Return(ctx, uint32_t(-1));
  }
  uint8_t* out = G(base, ctx.r5.u32);
  std::memset(out, 0, 16);
  Put16(out, 2);
  Put16(out + 2, ntohs(a.sin_port));
  uint32_t ip = ntohl(a.sin_addr.s_addr);
  if (!peer && ip == 0) ip = g_local;  // bound to any: report our Xbox address, like a console
  Put32(out + 4, ip);
  if (ctx.r6.u32) Put32(G(base, ctx.r6.u32), 16);
  Return(ctx, 0);
}
void GetSockName(PPCContext& ctx, uint8_t* base) { SockName(ctx, base, false); }
void GetPeerName(PPCContext& ctx, uint8_t* base) { SockName(ctx, base, true); }

// ---------------------------------------------------------------- quit diagnostics
// The game leaving on its own (no macOS crash report): log who asked.
void LogBacktrace(const char* why) {
  void* frames[48];
  const int n = backtrace(frames, 48);
  Log("%s; backtrace:", why);
  std::lock_guard lock(g_log_mutex);
  if (g_log) {
    backtrace_symbols_fd(frames, n, fileno(g_log));
    std::fflush(g_log);
  }
}

// Crashes: installed before the runtime's own handler, which saves it and passes
// on every fault it doesn't handle itself (guest MMIO faults stay with it). Logs
// the fault and the host frames (recompiled guest functions are named sub_XXXXXXXX),
// then lets the default action run (crash report).
void LogSymbol(const char* what, uintptr_t pc) {
  Dl_info info{};
  if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_sname) {
    Log("  %s %p = %s+0x%lx", what, reinterpret_cast<void*>(pc), info.dli_sname,
        static_cast<unsigned long>(pc - reinterpret_cast<uintptr_t>(info.dli_saddr)));
  } else {
    Log("  %s %p", what, reinterpret_cast<void*>(pc));
  }
}

void CrashHandler(int sig, siginfo_t* info, void* context) {
  auto* uc = static_cast<ucontext_t*>(context);
  Log("CRASH: signal %d at address %p", sig, info ? info->si_addr : nullptr);
  if (uc && uc->uc_mcontext) {
    LogSymbol("pc", uc->uc_mcontext->__ss.__pc);
    LogSymbol("lr", uc->uc_mcontext->__ss.__lr);
  }
  LogBacktrace("crash");
  signal(sig, SIG_DFL);
  if (sig == SIGABRT) raise(sig);
}

void InstallCrashHandler() {
  struct sigaction sa {};
  sa.sa_sigaction = CrashHandler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(sig, &sa, nullptr);
}

void TerminateTitle(PPCContext& ctx, uint8_t* base) {
  char why[96];
  std::snprintf(why, sizeof(why), "game quits: XamLoaderTerminateTitle from %08X", uint32_t(ctx.lr));
  LogBacktrace(why);
  Original_XamLoaderTerminateTitle(ctx, base);
}

void LaunchTitle(PPCContext& ctx, uint8_t* base) {
  char why[160];
  std::snprintf(why, sizeof(why), "game quits: XamLoaderLaunchTitle(\"%s\") from %08X",
                ctx.r3.u32 ? reinterpret_cast<const char*>(G(base, ctx.r3.u32)) : "dashboard", uint32_t(ctx.lr));
  LogBacktrace(why);
  Original_XamLoaderLaunchTitle(ctx, base);
}

void ExitHook(int code) {
  char why[48];
  std::snprintf(why, sizeof(why), "process exit(%d)", code);
  LogBacktrace(why);
  std::exit(code);
}
void UnderscoreExitHook(int code) {
  char why[48];
  std::snprintf(why, sizeof(why), "process _exit(%d)", code);
  LogBacktrace(why);
  _exit(code);
}
void AbortHook() {
  LogBacktrace("process abort()");
  std::abort();
}

// ---------------------------------------------------------------- online diagnostics
// Where the game's online start-up stops. DirtySock's NetConn (Xbox 360):
// NetConnStatus = sub_82A1C080(selector, data, buf, len); its update
// sub_82A1B310(ref) runs a state machine at ref+336 with the status code
// ('+onl', '-svc', ...) at ref+344. BlazeSDK's "Connecting to %s %s:%d" is in sub_82994560.
std::string FourCC(uint32_t v) {
  std::string s;
  for (int i = 24; i >= 0; i -= 8) {
    const char c = char(v >> i);
    s += (c >= 32 && c < 127) ? c : '.';
  }
  return s;
}

// NetConnStartup = sub_82A1B8E0(params) parses "-syslink" (ref+417), "-nosecure"
// (ref+419 = 0) and "-mixedsecure" (ref+420 = 0); the ref ends up at [0x830843C8].
// Secure (the default) reaches EA through an Xbox Live secure gateway (XLSP):
// NetConn wants the gateway's address at ref+28 and, without one, settles on
// '-svc' ("The EA server is not available"). On a LAN: -nosecure.
constexpr uint32_t kNetConnRef = 0x830843C8;  // lis -31992 = 0x8308
::PPCFunc* g_netconn_startup = nullptr;

void NetConnStartup(PPCContext& ctx, uint8_t* base) {
  const std::string params = ctx.r3.u32 ? reinterpret_cast<const char*>(G(base, ctx.r3.u32)) : "";
  g_netconn_startup(ctx, base);
  const uint32_t ref = Get32(G(base, kNetConnRef));
  Log("NetConnStartup(\"%s\") -> %d", params.c_str(), int(ctx.r3.u32));
  if (int32_t(ctx.r3.u32) == 0 && ref) {
    G(base, ref)[419] = 0;
    Log("NetConn: secure gateway off (-nosecure)");
  }
}

// BlazeSDK's debug print sub_829967F8(fmt, ...) formats into a buffer and hands it
// to a callback the release leaves empty. Formatted here instead, into lan.log.
::PPCFunc* g_blaze_print = nullptr;

void BlazePrint(PPCContext& ctx, uint8_t* base) {
  static std::atomic<int> budget{20000};
  if (budget.fetch_sub(1) <= 0 || !ctx.r3.u32) return g_blaze_print(ctx, base);
  const char* fmt = reinterpret_cast<const char*>(G(base, ctx.r3.u32));
  std::string out;
  int arg = 1;
  for (const char* f = fmt; *f && out.size() < 900; ++f) {
    if (*f != '%') {
      out += *f;
      continue;
    }
    std::string spec = "%";
    ++f;
    while (*f && std::strchr("-+ #0123456789.", *f)) spec += *f++;
    while (*f && std::strchr("hlLqjzt", *f)) ++f;  // every argument is one 64-bit register / slot
    if (!*f) break;
    char buf[300];
    const uint32_t v = Arg(ctx, base, arg);
    switch (*f) {
      case '%': out += '%'; continue;
      case 's': {
        ++arg;
        std::string s = v ? std::string(reinterpret_cast<const char*>(G(base, v)), strnlen((const char*)G(base, v), 200)) : "(null)";
        std::snprintf(buf, sizeof(buf), (spec + "s").c_str(), s.c_str());
        break;
      }
      case 'f': case 'g': case 'e': ++arg; std::snprintf(buf, sizeof(buf), "<float>"); break;
      case 'c': ++arg; std::snprintf(buf, sizeof(buf), (spec + "c").c_str(), int(v & 0xFF)); break;
      case 'd': case 'i': ++arg; std::snprintf(buf, sizeof(buf), (spec + "d").c_str(), int32_t(v)); break;
      case 'p': ++arg; std::snprintf(buf, sizeof(buf), "%08X", v); break;
      default: ++arg; std::snprintf(buf, sizeof(buf), (spec + *f).c_str(), v); break;  // u x X o
    }
    out += buf;
  }
  while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
  Log("[blaze] %s", out.c_str());
  g_blaze_print(ctx, base);
}

// ConnApi adapter ("[CANA]", vtable 0x82319018) keeps one map, ref+340, of mesh
// endpoints: addGame sub_829DFED0(ref, game) files the game under game->vfunc(+28),
// addPlayer sub_829DFFE0 / connectPlayer sub_829DFC88(ref, player) file players
// under player->vfunc(+12). A player looked up and finding the game's entry
// (player 0) calls address 0. Logs both keys.
uint32_t CallVirtual(const PPCContext& ctx, uint8_t* base, uint32_t object, uint32_t offset) {
  const uint32_t fn = Get32(G(base, Get32(G(base, object)) + offset));
  PPCContext call = ctx;
  call.r1.u64 = ctx.r1.u32 - 0x100;
  call.r3.u64 = object;
  rex::runtime::ResolveIndirectFunction(fn)(call, base);
  return call.r3.u32;
}

::PPCFunc* g_cana_add_game = nullptr;
::PPCFunc* g_cana_add_player = nullptr;
::PPCFunc* g_cana_connect_player = nullptr;

void CanaAddGame(PPCContext& ctx, uint8_t* base) {
  if (ctx.r4.u32) Log("CANA addGame: key %u", CallVirtual(ctx, base, ctx.r4.u32, 28));
  g_cana_add_game(ctx, base);
}
void CanaAddPlayer(PPCContext& ctx, uint8_t* base) {
  if (ctx.r4.u32) Log("CANA addPlayer: key %u", CallVirtual(ctx, base, ctx.r4.u32, 12));
  g_cana_add_player(ctx, base);
}
void CanaConnectPlayer(PPCContext& ctx, uint8_t* base) {
  if (ctx.r4.u32) Log("CANA connectPlayer: key %u", CallVirtual(ctx, base, ctx.r4.u32, 12));
  g_cana_connect_player(ctx, base);
}

// Xbox session creation (diagnostics): ConnApi host sub_82A23E98(ref, ...) ->
// sub_82A23538(ref) sends 'sess' to its DirtySessionManagers (control sub_82A2D928,
// which runs XSessionCreate) only when ref+360 (own client index) == ref+376
// (topology host index), else goes through sub_82A3B150. ref+684 = ConnApi state.
::PPCFunc* g_connapi_host = nullptr;
::PPCFunc* g_connapi_session = nullptr;
::PPCFunc* g_session_control = nullptr;
::PPCFunc* g_session_other = nullptr;

void ConnApiHost(PPCContext& ctx, uint8_t* base) {
  const uint8_t* r = G(base, ctx.r3.u32);
  Log("ConnApi host: state %u, self %u, topology host %u, flags %08X/%08X", Get32(r + 684), Get32(r + 360),
      Get32(r + 376), ctx.r6.u32, ctx.r7.u32);
  g_connapi_host(ctx, base);
}
void ConnApiSession(PPCContext& ctx, uint8_t* base) {
  const uint8_t* r = G(base, ctx.r3.u32);
  Log("ConnApi session start: self %u, topology host %u, state %u", Get32(r + 360), Get32(r + 376), Get32(r + 684));
  g_connapi_session(ctx, base);
  Log("ConnApi session start -> state %u", Get32(r + 684));
}
void SessionControl(PPCContext& ctx, uint8_t* base) {
  Track("DirtySessionManager control");
  const uint32_t sel = ctx.r4.u32, a = ctx.r5.u32, b = ctx.r6.u32;
  g_session_control(ctx, base);
  Log("DirtySessionManager control('%s', %u, %u) -> %d", FourCC(sel).c_str(), a, b,
      int(ctx.r3.u32));
}
void SessionOther(PPCContext& ctx, uint8_t* base) {
  Log("ConnApi session (other path) control('%s', %u, %u)", FourCC(ctx.r4.u32).c_str(), ctx.r5.u32, ctx.r6.u32);
  g_session_other(ctx, base);
}

// NetConn's per-user table (ref+80, 32 bytes per user: name[16], XUID at +16) is
// filled by sub_82A1AED8(entry, user) from the profile; DirtySessionManager looks
// the online XUID up in it (sub_82A1BBB0) before XSessionCreate. User 0 gets the
// launcher's name and XUID there too; the profile values are put back for the
// original's own compare so it doesn't see a change (and re-query) every call.
::PPCFunc* g_netconn_user = nullptr;

// XUserGetXUID wrapper sub_82EDE580(user, XUID*): the online code (BlazeSDK /
// DirtySock, 0x82980000..0x82A40000: xboxLogin, ConnApi, NetConn) gets the
// launcher's XUID; the game's own callers (profile, saves) keep the profile's.
::PPCFunc* g_get_xuid = nullptr;

void GetXuid(PPCContext& ctx, uint8_t* base) {
  const uint32_t user = ctx.r3.u32, out = ctx.r4.u32, lr = uint32_t(ctx.lr);
  g_get_xuid(ctx, base);
  if (ctx.r3.u32 == 0 && user == 0 && out && g_xuid && lr >= 0x82980000 && lr < 0x82A40000) {
    Put32(G(base, out), uint32_t(g_xuid >> 32));
    Put32(G(base, out) + 4, uint32_t(g_xuid));
  }
}

void NetConnUser(PPCContext& ctx, uint8_t* base) {
  const uint32_t entry = ctx.r3.u32, user = ctx.r4.u32;
  if (user != 0 || !g_xuid || !entry) return g_netconn_user(ctx, base);
  static uint8_t real[24];
  static bool have_real = false;
  uint8_t* e = G(base, entry);
  if (have_real && Get32(e + 16) == uint32_t(g_xuid >> 32) && Get32(e + 20) == uint32_t(g_xuid)) std::memcpy(e, real, 24);
  g_netconn_user(ctx, base);
  std::memcpy(real, e, 24);
  have_real = true;
  if (!g_name.empty()) {
    std::memset(e, 0, 16);
    std::memcpy(e, g_name.data(), std::min<size_t>(g_name.size(), 15));
  }
  Put32(e + 16, uint32_t(g_xuid >> 32));
  Put32(e + 20, uint32_t(g_xuid));
}

// ---------------------------------------------------------------- Xbox friends
// The game's friends menu lists Xbox Live friends: XFriendsCreateEnumerator
// (sub_82F395E0: user, start, count, size*, handle*) goes to the XLiveBase app
// (message 0x58020), which the runtime doesn't implement. Here it returns an
// enumerator of XONLINE_FRIEND (0xC4 bytes) holding everyone the LAN server knows,
// read from the server over HTTP (/skate3lan/friends).
::PPCFunc* g_friends_enum = nullptr;

struct LanFriend {
  uint64_t xuid;
  bool online, playing;
  std::string name;
};

constexpr uint32_t kSkate3TitleId = 0x454108E6;  // the friends menu only counts friends playing this title

// A pending game invite for this player: who sent it and their address (the server hands it out once).
struct LanInvite {
  uint64_t from = 0;
  uint32_t ip = 0;
};
std::mutex g_invite_mutex;
LanInvite g_invite;

// GET path on the LAN server's HTTP port; returns the body ("" when unreachable).
std::string HttpGet(const std::string& path) {
  std::string reply;
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return reply;
  timeval tv{1, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(80);
  to.sin_addr.s_addr = htonl(g_server);
  if (connect(fd, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0) {
    const std::string request = "GET " + path + " HTTP/1.0\r\n\r\n";
    send(fd, request.data(), request.size(), 0);
    char buf[2048];
    for (ssize_t n; (n = recv(fd, buf, sizeof(buf), 0)) > 0;) reply.append(buf, size_t(n));
  }
  close(fd);
  const size_t body = reply.find("\r\n\r\n");
  return body == std::string::npos ? std::string() : reply.substr(body + 4);
}

std::string XuidText(uint64_t xuid) {
  char text[17];
  std::snprintf(text, sizeof(text), "%016llX", static_cast<unsigned long long>(xuid));
  return text;
}

// Body lines: "<xuid hex> <online 0/1> <playing 0/1> <name>", plus "INVITE <xuid hex> <ip>" when
// someone invited this player.
std::vector<LanFriend> FetchFriends(LanInvite* invite = nullptr) {
  std::vector<LanFriend> out;
  const std::string body = HttpGet("/skate3lan/friends?me=" + XuidText(g_xuid));
  size_t pos = 0;
  while (pos < body.size()) {
    size_t end = body.find('\n', pos);
    if (end == std::string::npos) end = body.size();
    const std::string line = body.substr(pos, end - pos);
    pos = end + 1;
    unsigned long long xuid = 0;
    int online = 0, playing = 0, used = 0;
    char ip[32] = {};
    if (std::sscanf(line.c_str(), "INVITE %llx %31s", &xuid, ip) == 2) {
      in_addr a{};
      if (invite && inet_pton(AF_INET, ip, &a) == 1) *invite = {xuid, ntohl(a.s_addr)};
    } else if (std::sscanf(line.c_str(), "%llx %d %d %n", &xuid, &online, &playing, &used) == 3 && xuid) {
      out.push_back({xuid, online != 0, playing != 0, line.substr(size_t(used), 15)});
    }
  }
  return out;
}

// Every 2 s: when a friend comes online / starts or leaves a game, the game is told
// (XN_FRIENDS_PRESENCE_CHANGED) and enumerates again; an invite raises
// XN_LIVE_INVITE_ACCEPTED, which DirtySock's NetConn turns into its 'invt' status.
void FriendsPoller() {
  std::string last;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    auto* kernel = rex::system::kernel_state();
    if (!kernel) continue;
    LanInvite invite;
    const auto friends = FetchFriends(&invite);
    std::string state;
    for (const LanFriend& f : friends) state += XuidText(f.xuid) + char('0' + f.online) + char('0' + f.playing);
    if (state != last) {
      last = state;
      kernel->BroadcastNotification(0x04000001, 0);  // XN_FRIENDS_PRESENCE_CHANGED
    }
    if (invite.from) {
      {
        std::lock_guard lock(g_invite_mutex);
        g_invite = invite;
      }
      Log("invite from %s (%s): telling the game", XuidText(invite.from).c_str(), IpText(invite.ip).c_str());
      kernel->BroadcastNotification(0x02000002, 0);  // XN_LIVE_INVITE_ACCEPTED
    }
  }
}

// XShowGameInviteUI(user, xuids, count, text): the Guide would send the invites; here the server does.
void ShowGameInviteUI(PPCContext& ctx, uint8_t* base) {
  const uint32_t xuids = ctx.r4.u32, count = ctx.r5.u32;
  Log("invite UI: %u recipient(s)", count);
  for (uint32_t i = 0; xuids && i < count && i < 16; ++i) {
    const uint8_t* x = G(base, xuids + i * 8);
    const uint64_t to = uint64_t(Get32(x)) << 32 | Get32(x + 4);
    HttpGet("/skate3lan/invite?from=" + XuidText(g_xuid) + "&to=" + XuidText(to));
    Log("invited %s", XuidText(to).c_str());
  }
  Return(ctx, 0);
}

// XInviteGetAcceptedInfo (sub_82F39740: user, XINVITE_INFO*): the invite the poller picked up.
// XINVITE_INFO: xuidInvitee, xuidInviter, dwTitleID, XSESSION_INFO {XNKID, XNADDR host, XNKEY}, fFromGameInvite.
::PPCFunc* g_invite_info = nullptr;

void InviteAcceptedInfo(PPCContext& ctx, uint8_t* base) {
  const uint32_t info = ctx.r4.u32;
  LanInvite invite;
  {
    std::lock_guard lock(g_invite_mutex);
    invite = g_invite;
  }
  if (!invite.from || !info) return g_invite_info(ctx, base);
  uint8_t* p = G(base, info);
  std::memset(p, 0, 84);
  Put32(p, uint32_t(g_xuid >> 32));
  Put32(p + 4, uint32_t(g_xuid));
  Put32(p + 8, uint32_t(invite.from >> 32));
  Put32(p + 12, uint32_t(invite.from));
  Put32(p + 16, kSkate3TitleId);
  Put32(p + 20, invite.ip);  // session id: the host's address, unique enough on a LAN
  FillXnAddr(p + 28, invite.ip);
  Put32(p + 80, 1);
  Log("invite accepted: joining %s", XuidText(invite.from).c_str());
  Return(ctx, 0);
}

void FriendsCreateEnumerator(PPCContext& ctx, uint8_t* base) {
  const uint32_t user = ctx.r3.u32, start = ctx.r4.u32, count = ctx.r5.u32;
  const uint32_t size_out = ctx.r6.u32, handle_out = ctx.r7.u32;
  if (user > 3 || count == 0 || count > 100 || !handle_out) return g_friends_enum(ctx, base);
  constexpr uint32_t kFriendSize = 0xC4;
  auto* kernel = rex::system::kernel_state();
  auto e = rex::system::object_ref<rex::system::XStaticUntypedEnumerator>(
      new rex::system::XStaticUntypedEnumerator(kernel, count, kFriendSize));
  if (XFAILED(e->Initialize(user, 0xFC, 0x58020, 0x58021, 0))) return Return(ctx, 1627);
  const auto friends = FetchFriends();
  for (size_t i = start; i < friends.size(); ++i) {
    const LanFriend& f = friends[i];
    uint8_t* item = e->AppendItem();
    std::memset(item, 0, kFriendSize);
    Put32(item, uint32_t(f.xuid >> 32));  // xuid
    Put32(item + 4, uint32_t(f.xuid));
    std::memcpy(item + 8, f.name.data(), std::min<size_t>(f.name.size(), 15));  // szGamertag[16]
    // dwFriendState: online, playing this title, joinable (no request flags = accepted friend).
    Put32(item + 24, f.online ? (0x1 | (f.playing ? 0x2 | 0x10 : 0)) : 0);
    if (f.online) Put32(item + 36, kSkate3TitleId);  // dwTitleID
  }
  if (size_out) Put32(G(base, size_out), count * kFriendSize);
  Put32(G(base, handle_out), e->handle());
  LogOnce("Xbox friends enumerated:", uint32_t(friends.size()), false);
  Return(ctx, 0);
}

// ---------------------------------------------------------------- online menu
// The single player crossbar's Online menu (model 0x82303D54) reads its options from
// row 1 of a static table (0x82205A68, 8 option ids per row, -1 ends a row): Online
// Challenge Map, Online Team, skate.Profile: Online, Contacts, Xbox LIVE Party,
// Leaderboards. Every model slot (count, id, enabled) indexes that row, so the row is
// rewritten in guest memory to keep only Challenge Map and Contacts.
// The in-session Online menus (host / client, team, ranked, lobby models) have rows of
// their own in the tables around it (0x82205800..0x82206400); Xbox LIVE Party (38)
// is cut out of each of those -1 terminated rows.
void RemovePartyOption(uint8_t* tables) {
  constexpr uint32_t kParty = 38, kEnd = 0xFFFFFFFF, kOptionCount = 49;
  int removed = 0;
  for (uint32_t at = 0; at + 32 <= 0xC00; at += 4) {
    uint8_t* p = tables + at;
    if (Get32(p) != kParty) continue;
    int end = 1;
    while (end < 8 && Get32(p + 4 * end) < kOptionCount) ++end;
    if (end == 8 || Get32(p + 4 * end) != kEnd) continue;  // not a menu row
    for (int i = 0; i < end; ++i) Put32(p + 4 * i, Get32(p + 4 * (i + 1)));
    ++removed;
  }
  Log("online menu: Party removed from %d in-session menu(s)", removed);
}

// Contacts (option 36, sub-option set 1: Teammates, Friends, Recent Players, Invite
// Friend) keeps only the friends list, named "Server Players". Sets live at
// 0x83027958 (24 bytes: label pointers), their help texts 0xA8 further. The crossbar
// screen asks sub_8261DB48(screen, menu, option) for the count and runs the picked
// sub-option in sub_8261E4A0(screen, ?, index) (index 1 = friends; called through the
// function table only); the current menu / option sit at screen+368 / +372 and the
// mode's model at +480 maps them to an option id (vtable +8).
constexpr uint32_t kOptionTable = 0x83027558, kContactsSet = 1;
constexpr uint32_t kContactsLabels = 0x83027958 + 24 * kContactsSet, kContactsHelp = kContactsLabels + 0xA8;
::PPCFunc* g_suboption_count = nullptr;
::PPCFunc* g_suboption_picked = nullptr;

uint32_t SubOptionSet(const PPCContext& ctx, uint8_t* base, uint32_t screen, uint32_t menu, uint32_t option) {
  const uint32_t model = Get32(G(base, screen + 480));
  if (!model) return ~0u;
  PPCContext call = ctx;
  call.r1.u64 = ctx.r1.u32 - 0x100;
  call.r3.u64 = model;
  call.r4.u64 = menu;
  call.r5.u64 = option;
  rex::runtime::ResolveIndirectFunction(Get32(G(base, Get32(G(base, model)) + 8)))(call, base);
  return call.r3.u32 < 49 ? Get32(G(base, kOptionTable + 20 * call.r3.u32 + 12)) : ~0u;
}

void SubOptionCount(PPCContext& ctx, uint8_t* base) {
  const uint32_t screen = ctx.r3.u32, menu = ctx.r4.u32, option = ctx.r5.u32;
  g_suboption_count(ctx, base);
  if (SubOptionSet(ctx, base, screen, menu, option) == kContactsSet) Return(ctx, 1);
}

void SubOptionPicked(PPCContext& ctx, uint8_t* base) {
  const uint32_t screen = ctx.r3.u32;
  if (ctx.r5.u32 == 0 &&
      SubOptionSet(ctx, base, screen, Get32(G(base, screen + 368)), Get32(G(base, screen + 372))) == kContactsSet) {
    ctx.r5.u64 = 1;  // the only row is the friends list
  }
  g_suboption_picked(ctx, base);
}

void TrimContacts(rex::Runtime* runtime) {
  auto* memory = runtime->memory();
  auto* dispatcher = runtime->function_dispatcher();
  constexpr char kLabel[] = "#Server Players";  // '#': shown as written, not looked up
  const uint32_t label = Alloc(sizeof(kLabel));
  if (!label || !g_suboption_count || !(g_suboption_picked = dispatcher->GetFunction(0x8261E4A0))) {
    Log("contacts: hooks missing, menu left as is");
    return;
  }
  std::memcpy(memory->TranslateVirtual<uint8_t*>(label), kLabel, sizeof(kLabel));
  uint8_t* labels = memory->TranslateVirtual<uint8_t*>(kContactsLabels);
  uint8_t* help = memory->TranslateVirtual<uint8_t*>(kContactsHelp);
  Put32(labels, label);
  Put32(help, Get32(help + 4));
  dispatcher->SetFunction(0x8261E4A0, SubOptionPicked);
  Log("contacts: only Server Players (friends list)");
}

void TrimOnlineMenu() {
  constexpr uint32_t kRow = 0x82205A88;
  constexpr uint32_t kStock[] = {2, 26, 11, 36, 38, 30, 0xFFFFFFFF};
  constexpr uint32_t kTrimmed[] = {2, 36, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
  for (int tries = 0; tries < 2400; ++tries, std::this_thread::sleep_for(std::chrono::milliseconds(50))) {
    auto* runtime = rex::Runtime::instance();
    auto* memory = runtime ? runtime->memory() : nullptr;
    if (!memory || !runtime->function_dispatcher()) continue;
    uint8_t* row = memory->TranslateVirtual<uint8_t*>(kRow);
    bool stock = true;
    for (size_t i = 0; i < std::size(kStock); ++i) stock &= Get32(row + 4 * i) == kStock[i];
    if (!stock) continue;  // image not loaded yet (the runtime keeps image pages writable)
    for (size_t i = 0; i < std::size(kTrimmed); ++i) Put32(row + 4 * i, kTrimmed[i]);
    Log("online menu: Online Team, skate.Profile: Online, Party and Leaderboards removed");
    RemovePartyOption(memory->TranslateVirtual<uint8_t*>(0x82205800));
    TrimContacts(runtime);
    return;
  }
  Log("online menu: table not found, left as is");
}

// ---------------------------------------------------------------- title logo
// The press-start screen spells "skate3" with 8 textures (letters s, ka, te, 3 and
// their bottom rows, bootflow/pressstart.rx2). Every texture the game loads passes
// 0x82A77310 (resolve: points the header's fetch constant at the raster, called
// only through the loader's registry, so swapped in the function table). Once the
// original ran, a stock letter raster (matched by hash) is repointed at our
// "skate.live" raster (title_logo.h).
constexpr uint32_t kTextureResolve = 0x82A77310;
::PPCFunc* g_texture_resolve = nullptr;
uint8_t* g_physbase = nullptr;
uint32_t g_logo_phys[std::size(kTitleLogo)] = {};

uint64_t Fnv1a(const uint8_t* p, size_t n) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
  return h;
}

void TextureResolveHook(PPCContext& ctx, uint8_t* base) {
  uint8_t* fetch = G(base, ctx.r3.u32 + 0x1C);
  g_texture_resolve(ctx, base);
  const uint32_t word1 = Get32(fetch + 4), phys = word1 & 0x1FFFF000, dims = Get32(fetch + 8);
  if ((word1 & 0xFFF) != 0x054) return;  // DXT5, 8-in-16
  for (size_t i = 0; i < std::size(kTitleLogo); ++i) {
    const TitleLogoPiece& piece = kTitleLogo[i];
    if (piece.fetch_size != dims || phys + piece.size > 0x20000000 ||
        Fnv1a(g_physbase + phys, piece.size) != piece.stock_hash)
      continue;
    Put32(fetch + 4, (word1 & 0xE0000FFF) | g_logo_phys[i]);
    if (i == 0) Log("title logo: now reads skate.live");
    return;
  }
}

// The function table and physical memory exist once the runtime is up: polled
// from the constructor's thread, well before the press-start screen loads.
void InstallTitleLogo() {
  for (int tries = 0; tries < 6000; ++tries) {
    auto* runtime = rex::Runtime::instance();
    auto* dispatcher = runtime ? runtime->function_dispatcher() : nullptr;
    if (dispatcher && runtime->memory() && (g_texture_resolve = dispatcher->GetFunction(kTextureResolve))) {
      auto* memory = runtime->memory();
      g_physbase = memory->physical_membase();
      for (size_t i = 0; i < std::size(kTitleLogo); ++i) {
        const uint32_t at = memory->SystemHeapAlloc(kTitleLogo[i].size, 0x1000, rex::memory::kSystemHeapPhysical);
        // The physical heap's 0xE0000000 view is 0x1000 off its physical address:
        // ask the runtime instead of masking, or the raster lands next to our block.
        const uint32_t phys = at ? memory->GetPhysicalAddress(at) : UINT32_MAX;
        if (phys == UINT32_MAX) {
          Log("title logo: no physical memory, keeping skate3");
          return;
        }
        g_logo_phys[i] = phys;
        std::memcpy(memory->TranslateVirtual<uint8_t*>(at), kTitleLogo[i].raster, kTitleLogo[i].size);
      }
      dispatcher->SetFunction(kTextureResolve, TextureResolveHook);
      Log("title logo hook ready");
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  Log("title logo: runtime not found, keeping skate3");
}

// ---------------------------------------------------------------- menu names
// Every "Xbox LIVE" in the game's texts reads "Skate.live" (menus, prompts, errors).
// Menu text comes from the language table (miscboot.big: fe/languages/<lang>/
// LANGUAGE_<Lang>_Global_skate3ng.BIN), loaded whole at boot, byte-swapped in place
// (the file is little-endian) and relocated: magic 0x00039000, "Global" at +0x14,
// {hash, string pointer} entries at 8 + [+0xC] (offsets from 8 + [+0x10] in the
// file). Every language spells it "Xbox LIVE".

// Every text naming Xbox LIVE gets a copy with "Skate.live" instead (written to
// guest memory from Alloc); the entry then points at the copy.
std::string ReplaceXboxLive(std::string text) {
  for (const char* from : {"Xbox LIVE", "Xbox Live"}) {
    for (size_t at; (at = text.find(from)) != std::string::npos;) text.replace(at, std::strlen(from), "Skate.live");
  }
  return text;
}

void PatchLanguageTable(uint8_t* base, uint8_t* table, uint32_t guest) {
  const uint32_t count = Get32(table + 8), entries = 8 + Get32(table + 12), size = Get32(table + 4) + 8;
  if (count == 0 || count > 200000 || entries + count * 8 > size) return;
  std::map<uint32_t, std::string> replaced;  // old string pointer -> new text
  size_t total = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t ptr = Get32(table + entries + i * 8 + 4), at = ptr - guest;  // offsets were relocated to pointers
    if (at >= size || replaced.count(ptr)) continue;
    const char* text = reinterpret_cast<const char*>(table + at);
    const size_t len = strnlen(text, size - at);
    if (!memmem(text, len, "Xbox L", 6)) continue;
    std::string fixed = ReplaceXboxLive(std::string(text, len));
    if (fixed.size() == len) continue;
    total += fixed.size() + 1;
    replaced.emplace(ptr, std::move(fixed));
  }
  if (replaced.empty()) return;
  const uint32_t block = Alloc(uint32_t(total));
  if (!block) return;
  std::map<uint32_t, uint32_t> moved;
  uint32_t next = block;
  for (const auto& [ptr, text] : replaced) {
    std::memcpy(G(base, next), text.c_str(), text.size() + 1);
    moved[ptr] = next;
    next += uint32_t(text.size() + 1);
  }
  int patched = 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t* entry = table + entries + i * 8;
    const auto it = moved.find(Get32(entry + 4));
    if (it == moved.end()) continue;
    Put32(entry + 4, it->second);
    ++patched;
  }
  Log("menu names: language table at %08X, %d texts now say Skate.live", guest, patched);
}

// The loader (sub_82D083D8 -> sub_8290C2A8) swaps the header, relocates the
// entries in sub_8290D638 (r3 = table) and then copies strings into its caches,
// so the entries are patched right after the relocation.
::PPCFunc* g_language_relocate = nullptr;

void LanguageRelocateHook(PPCContext& ctx, uint8_t* base) {
  const uint32_t table = ctx.r3.u32;
  g_language_relocate(ctx, base);
  uint8_t* host = G(base, table);
  if (Get32(host) != 0x00039000 || std::memcmp(host + 0x14, "Global", 7) != 0) return;
  PatchLanguageTable(base, host, table);
}

::PPCFunc* g_netconn_status = nullptr;
::PPCFunc* g_netconn_update = nullptr;
::PPCFunc* g_blaze_connect = nullptr;

void NetConnStatus(PPCContext& ctx, uint8_t* base) {
  const uint32_t selector = ctx.r3.u32, lr = uint32_t(ctx.lr), buf = ctx.r5.u32, len = ctx.r6.u32;
  g_netconn_status(ctx, base);
  // Online identity (Blaze login, ConnApi, Xbox client address): the launcher's name and XUID.
  if (selector == 0x78756964 && g_xuid && buf && len >= 8) {  // 'xuid': 0 = ok
    Put32(G(base, buf), uint32_t(g_xuid >> 32));
    Put32(G(base, buf) + 4, uint32_t(g_xuid));
    Return(ctx, 0);
  } else if (selector == 0x67746167 && !g_name.empty() && buf && len > 1) {  // 'gtag': nonzero = ok
    const size_t n = std::min<size_t>(g_name.size(), std::min<uint32_t>(len - 1, 15));
    std::memcpy(G(base, buf), g_name.data(), n);
    G(base, buf)[n] = 0;
    Return(ctx, 1);
  }
  static std::mutex m;
  static std::map<uint32_t, uint32_t> last;
  std::lock_guard lock(m);
  auto [it, fresh] = last.try_emplace(selector, ctx.r3.u32);
  if (fresh || it->second != ctx.r3.u32) {
    it->second = ctx.r3.u32;
    Log("NetConnStatus('%s') -> %08X '%s' (from %08X)", FourCC(selector).c_str(), ctx.r3.u32, FourCC(ctx.r3.u32).c_str(), lr);
  }
}

void NetConnUpdate(PPCContext& ctx, uint8_t* base) {
  Track("NetConn update");
  const uint32_t ref = ctx.r3.u32;
  // -nosecure on every tick: NetConn is also restarted through the function
  // table, which skips the NetConnStartup hook.
  G(base, ref)[419] = 0;
  g_netconn_update(ctx, base);
  static uint32_t last_state = ~0u, last_status = ~0u;
  const uint32_t state = Get32(G(base, ref + 336)), status = Get32(G(base, ref + 344));
  if (state != last_state || status != last_status) {
    last_state = state;
    last_status = status;
    Log("NetConn state %u, status '%s' (flags 417..423: %u %u %u %u %u %u %u)", state, FourCC(status).c_str(),
        G(base, ref)[417], G(base, ref)[418], G(base, ref)[419], G(base, ref)[420], G(base, ref)[421],
        G(base, ref)[422], G(base, ref)[423]);
  }
}

void BlazeConnect(PPCContext& ctx, uint8_t* base) {
  Log("BlazeSDK: connecting (from %08X)", uint32_t(ctx.lr));
  g_blaze_connect(ctx, base);
}

void XMsgLog(const char* kind, uint32_t app, uint32_t msg, uint32_t result) {
  static std::mutex m;
  static std::set<std::tuple<uint32_t, uint32_t, uint32_t>> seen;
  std::lock_guard lock(m);
  if (seen.insert({app, msg, result}).second) Log("%s app %X msg %08X -> %08X", kind, app, msg, result);
}
void XMsgStart(PPCContext& ctx, uint8_t* base) {
  const uint32_t app = ctx.r3.u32, msg = ctx.r4.u32;
  Original_XMsgStartIORequest(ctx, base);
  XMsgLog("XMsgStartIORequest", app, msg, ctx.r3.u32);
}
void XMsgStartEx(PPCContext& ctx, uint8_t* base) {
  const uint32_t app = ctx.r3.u32, msg = ctx.r4.u32;
  Original_XMsgStartIORequestEx(ctx, base);
  XMsgLog("XMsgStartIORequestEx", app, msg, ctx.r3.u32);
}
void XMsgCall(PPCContext& ctx, uint8_t* base) {
  const uint32_t app = ctx.r3.u32, msg = ctx.r4.u32;
  Original_XMsgInProcessCall(ctx, base);
  XMsgLog("XMsgInProcessCall", app, msg, ctx.r3.u32);
}

// ---------------------------------------------------------------- overlapped receive
// The runtime has no overlapped I/O: WSARecvFrom always fails, WSARecv and
// WSAGetOverlappedResult are stubs. DirtySock's socket thread (sub_82A25BE0)
// posts an overlapped WSARecvFrom per socket and waits on their events forever,
// holding a lock: with peer traffic both games froze. Here every overlapped
// receive goes pending (WSA_IO_PENDING) and a poller thread completes it from the
// host socket: data and sender into guest memory, the overlapped's Internal /
// InternalHigh set, its event signaled. XWSAOVERLAPPED: Internal, InternalHigh,
// Offset, OffsetHigh, hEvent; XWSABUF: len, buf.
constexpr uint32_t kStatusPending = 0x103;

struct PendingRecv {
  uint32_t handle;
  int fd;
  std::vector<std::pair<uint32_t, uint32_t>> buffers;  // guest address, length
  uint32_t from, fromlen, overlapped, event;
};
std::mutex g_recv_mutex;
std::vector<PendingRecv> g_pending;
std::map<uint32_t, uint32_t> g_recv_errors;  // overlapped -> Winsock error
uint8_t* g_base = nullptr;
int g_wake[2] = {-1, -1};  // poller wake-up pipe: a new receive went pending

void CompleteRecv(const PendingRecv& op, const uint8_t* data, int n, int error) {
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 40) Log("recv done: socket %u, %d bytes, error %d", op.handle, n, n < 0 ? error : 0);
  uint8_t* o = G(g_base, op.overlapped);
  if (n >= 0) {
    g_recv_packets++;
    g_recv_bytes += uint32_t(n);
    int copied = 0;
    for (auto [addr, len] : op.buffers) {
      const int part = std::min<int>(int(len), n - copied);
      if (part <= 0) break;
      std::memcpy(G(g_base, addr), data + copied, size_t(part));
      copied += part;
    }
    Put32(o + 4, uint32_t(copied));
    Put32(o, 0);
  } else {
    {
      std::lock_guard lock(g_recv_mutex);
      g_recv_errors[op.overlapped] = WsaError(error);
    }
    Put32(o + 4, 0);
    Put32(o, 0xC0000001);  // STATUS_UNSUCCESSFUL
  }
  SetEvent(op.event);
}

// Receives one datagram for op if one is waiting: false = nothing there yet.
bool TryRecv(const PendingRecv& op) {
  static thread_local std::vector<uint8_t> buffer(65536);
  sockaddr_in from{};
  socklen_t fromlen = sizeof(from);
  const ssize_t n = recvfrom(op.fd, buffer.data(), buffer.size(), MSG_DONTWAIT, (sockaddr*)&from, &fromlen);
  const int e = errno;
  if (n < 0 && (e == EAGAIN || e == EWOULDBLOCK)) return false;
  if (n >= 0 && op.from) {
    uint8_t* f = G(g_base, op.from);
    std::memset(f, 0, 16);
    Put16(f, 2);
    Put16(f + 2, ntohs(from.sin_port));
    Put32(f + 4, ntohl(from.sin_addr.s_addr));
    if (op.fromlen) Put32(G(g_base, op.fromlen), 16);
  }
  CompleteRecv(op, buffer.data(), int(n), e);
  return true;
}

void RecvPoller() {
  for (;;) {
    std::vector<pollfd> fds{{g_wake[0], POLLIN, 0}};
    {
      std::lock_guard lock(g_recv_mutex);
      for (auto& op : g_pending) fds.push_back({op.fd, POLLIN, 0});
    }
    if (poll(fds.data(), nfds_t(fds.size()), 100) <= 0) continue;
    if (fds[0].revents & POLLIN) {
      char drain[64];
      while (read(g_wake[0], drain, sizeof(drain)) > 0) {
      }
    }
    for (size_t i = 1; i < fds.size(); ++i) {
      if (!(fds[i].revents & (POLLIN | POLLERR | POLLHUP))) continue;
      PendingRecv op;
      {
        std::lock_guard lock(g_recv_mutex);
        auto it = std::find_if(g_pending.begin(), g_pending.end(), [&](const PendingRecv& o) { return o.fd == fds[i].fd; });
        if (it == g_pending.end()) continue;
        op = *it;
        g_pending.erase(it);
      }
      if (!TryRecv(op)) {  // spurious: back in the queue
        std::lock_guard lock(g_recv_mutex);
        g_pending.push_back(op);
      }
    }
  }
}

// Shared by WSARecvFrom / WSARecv: queue the receive, report WSA_IO_PENDING.
void PostRecv(PPCContext& ctx, uint8_t* base, uint32_t handle, uint32_t bufs, uint32_t count, uint32_t from,
              uint32_t fromlen, uint32_t overlapped) {
  static std::once_flag once;
  static std::atomic<int> logged{0};
  g_base = base;
  Track("WSARecv(From)");
  if (logged.fetch_add(1) < 20) Log("recv post: socket %u, overlapped %08X, event %08X", handle, overlapped, Get32(G(base, overlapped) + 16));
  std::call_once(once, [] {
    if (pipe(g_wake) == 0) {
      fcntl(g_wake[0], F_SETFL, O_NONBLOCK);
      fcntl(g_wake[1], F_SETFL, O_NONBLOCK);
    }
    std::thread(RecvPoller).detach();
  });
  const int fd = NativeSocket(handle);
  if (fd < 0) {
    rex::system::XThread::SetLastError(10038);  // WSAENOTSOCK
    return Return(ctx, uint32_t(-1));
  }
  PendingRecv op{handle, fd, {}, from, fromlen, overlapped, Get32(G(base, overlapped) + 16)};
  for (uint32_t i = 0; i < count && i < 16; ++i) {
    const uint8_t* b = G(base, bufs + 8 * i);
    op.buffers.push_back({Get32(b + 4), Get32(b)});
  }
  Put32(G(base, overlapped), kStatusPending);
  Put32(G(base, overlapped) + 4, 0);
  {
    std::lock_guard lock(g_recv_mutex);
    g_recv_errors.erase(overlapped);
  }
  // Data already waiting (a burst): completed right here, event set; still reported as
  // pending, which the game handles the same way. Otherwise the poller takes over now.
  if (!TryRecv(op)) {
    {
      std::lock_guard lock(g_recv_mutex);
      g_pending.push_back(std::move(op));
    }
    const char wake = 1;
    if (g_wake[1] >= 0) (void)write(g_wake[1], &wake, 1);
  }
  rex::system::XThread::SetLastError(997);  // WSA_IO_PENDING
  Return(ctx, uint32_t(-1));
}

// (caller, s, bufs, count, bytes*, flags*, from*, fromlen*, overlapped*, completion)
void WsaRecvFrom(PPCContext& ctx, uint8_t* base) {
  const uint32_t overlapped = Arg(ctx, base, 8);
  if (!overlapped) {  // blocking form: one plain recvfrom into the first buffer
    rex::system::XThread::SetLastError(10045);  // WSAEOPNOTSUPP
    return Return(ctx, uint32_t(-1));
  }
  PostRecv(ctx, base, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r9.u32, ctx.r10.u32, overlapped);
}

// (caller, s, bufs, count, bytes*, flags*, overlapped*, completion)
void WsaRecv(PPCContext& ctx, uint8_t* base) {
  if (!ctx.r9.u32) {
    rex::system::XThread::SetLastError(10045);
    return Return(ctx, uint32_t(-1));
  }
  PostRecv(ctx, base, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 0, 0, ctx.r9.u32);
}

// (caller, s, overlapped*, bytes*, wait, flags*) -> TRUE / FALSE
void WsaGetOverlappedResult(PPCContext& ctx, uint8_t* base) {
  Track("WSAGetOverlappedResult");
  const uint32_t overlapped = ctx.r5.u32, bytes = ctx.r6.u32, wait = ctx.r7.u32, flags = ctx.r8.u32;
  if (!overlapped) return Return(ctx, 0);
  uint8_t* o = G(base, overlapped);
  for (int i = 0; Get32(o) == kStatusPending; ++i) {
    if (!wait || i > 5000) {
      rex::system::XThread::SetLastError(996);  // WSA_IO_INCOMPLETE
      return Return(ctx, 0);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (bytes) Put32(G(base, bytes), Get32(o + 4));
  if (flags) Put32(G(base, flags), 0);
  if (Get32(o) != 0) {
    uint32_t error = 10054;
    {
      std::lock_guard lock(g_recv_mutex);
      if (auto it = g_recv_errors.find(overlapped); it != g_recv_errors.end()) error = it->second;
    }
    rex::system::XThread::SetLastError(error);
    return Return(ctx, 0);
  }
  Return(ctx, 1);
}

void CloseSocket(PPCContext& ctx, uint8_t* base) {
  {
    std::lock_guard lock(g_recv_mutex);
    std::erase_if(g_pending, [&](const PendingRecv& op) { return op.handle == ctx.r4.u32; });
  }
  Original_NetDll_closesocket(ctx, base);
}

// ---------------------------------------------------------------- scripted pad input (testing)
// SKATE3_LAN_INPUT=<file>: every line appended to the file is one pad press played
// into the game (a b x y start back lb rb lt rt up down left right; "wait <ms>"),
// so several games on one Mac can be driven without focus.
void InputScript(std::string path) {
  struct Token { const char* name; uint16_t buttons; uint8_t lt, rt; };
  static constexpr Token kTokens[] = {
      {"up", 0x0001, 0, 0},   {"down", 0x0002, 0, 0},  {"left", 0x0004, 0, 0}, {"right", 0x0008, 0, 0},
      {"start", 0x0010, 0, 0}, {"back", 0x0020, 0, 0}, {"lb", 0x0100, 0, 0},   {"rb", 0x0200, 0, 0},
      {"a", 0x1000, 0, 0},    {"b", 0x2000, 0, 0},     {"x", 0x4000, 0, 0},    {"y", 0x8000, 0, 0},
      {"lt", 0, 255, 0},      {"rt", 0, 0, 255},
  };
  long offset = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) continue;
    std::fseek(f, offset, SEEK_SET);
    char line[64];
    while (std::fgets(line, sizeof(line), f)) {
      offset = std::ftell(f);
      std::string t(line);
      t.erase(t.find_last_not_of(" \r\n\t") + 1);
      if (t.rfind("wait ", 0) == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(t.c_str() + 5)));
        continue;
      }
      for (const Token& k : kTokens) {
        if (t == k.name) {
          rex::kernel::xam::QueueSyntheticInput(k.buttons, k.lt, k.rt, (k.lt || k.rt) ? 16 : 8);
          Log("input: %s", k.name);
          std::this_thread::sleep_for(std::chrono::milliseconds(400));
        }
      }
    }
    std::fclose(f);
  }
}

// DirtySock's global NetCrit (0x830708A0: owner thread id, count, busy flag, then
// the RTL critical section the owner holds): logged when it stays taken.
void TrafficLog() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const uint32_t sp = g_sent_packets.exchange(0), sb = g_sent_bytes.exchange(0), rp = g_recv_packets.exchange(0),
                   rb = g_recv_bytes.exchange(0), se = g_send_errors.exchange(0);
    if (sp || rp || se) Log("net: sent %u packets (%u bytes), received %u (%u bytes), send errors %u", sp, sb, rp, rb, se);
  }
}

void LockWatchdog() {
  uint32_t last_owner = 0;
  int same = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    if (!g_base) continue;
    const uint8_t* c = G(g_base, 0x830708A0);  // lis -31993 = 0x8307
    const uint32_t owner = Get32(c), count = Get32(c + 4), busy = Get32(c + 8);
    same = (owner && owner == last_owner) ? same + 1 : 0;
    last_owner = owner;
    if (same == 2) {
      std::string last;
      {
        std::lock_guard lock(g_track_mutex);
        if (auto it = g_last_call.find(owner); it != g_last_call.end()) last = it->second;
      }
      Log("watchdog: NetCrit held by thread %08X for 4 s (count %u, busy %u, its last network call: %s)", owner, count,
          busy, last.empty() ? "none seen" : last.c_str());
      std::lock_guard lock(g_track_mutex);
      for (auto& [tid, what] : g_last_call) Log("  thread %08X last: %s", tid, what.c_str());
    }
  }
}

// ---------------------------------------------------------------- BlazeHub::initialize
// sub_829953A0(InitParameters*, allocator), called once from 0x827388E4.
// InitParameters: +0 ClientName, +64 ClientVersion, +128 ClientSkuId, +192
// ServiceName (char[64] each), +680 Secure, +684 Environment, +691 EnableQos, +692
// RedirectorAddress override (char[256]), +948 RedirectorPort override.
// Found from its parameter dump sub_829951B8 ("BlazeHub Parameters:").
::PPCFunc* g_blaze_init = nullptr;

void BlazeInit(PPCContext& ctx, uint8_t* base) {
  g_base = base;
  uint8_t* p = G(base, ctx.r3.u32);
  auto text = [p](int off) { return std::string(reinterpret_cast<const char*>(p + off), strnlen((const char*)p + off, 64)); };
  Log("BlazeHub: client \"%s\" %s, sku %s, service \"%s\", environment %u, secure %u, qos %u", text(0).c_str(),
      text(64).c_str(), text(128).c_str(), text(192).c_str(), Get32(p + 684), p[680], p[691]);
  p[680] = 0;
  // EnableQos off: with it on, creating or joining a game first needs ping-site
  // latencies measured against EA's Xbox Live (XLSP) QoS servers
  // (ConnectionManager vtable+96, sub_82A0C660 = latency sent || !EnableQos),
  // else SDK_ERR_QOS_PINGSITE_NOT_INITIALIZED ("Failed to join or create an online session").
  p[691] = 0;
  std::snprintf(reinterpret_cast<char*>(p + 692), 256, "%s", g_server_text);
  Put16(p + 948, kBlazePort);
  Log("BlazeHub: redirector -> %s:%u, plain TCP", g_server_text, kBlazePort);
  g_blaze_init(ctx, base);
  Log("BlazeHub::initialize -> %d", int(ctx.r3.u32));
}

// ---------------------------------------------------------------- direct-call hook
// The release's guest functions are exported one-instruction wrappers
// (`_sub_X: b ___imp__sub_X`) that every direct call goes through. HookDirect
// points the wrapper at `hook` through a trampoline in branch range and returns
// the original body.
void* AllocateNear(uintptr_t near) {
  const mach_vm_size_t page = vm_page_size;
  for (mach_vm_address_t addr = (near + page) & ~(page - 1); addr < near + (120u << 20);) {
    mach_vm_address_t region = addr;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    const kern_return_t kr = mach_vm_region(mach_task_self(), &region, &size, VM_REGION_BASIC_INFO_64,
                                            reinterpret_cast<vm_region_info_t>(&info), &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    if (kr != KERN_SUCCESS || region > addr) {
      mach_vm_address_t want = addr;
      if (mach_vm_allocate(mach_task_self(), &want, page, VM_FLAGS_FIXED) == KERN_SUCCESS) {
        return reinterpret_cast<void*>(want);
      }
      if (kr != KERN_SUCCESS) break;
    }
    addr = region + size;
  }
  return nullptr;
}

::PPCFunc* HookDirect(const char* name, ::PPCFunc* hook) {
  const auto original = reinterpret_cast<::PPCFunc*>(dlsym(RTLD_DEFAULT, (std::string("__imp__") + name).c_str()));
  auto* wrapper = static_cast<uint32_t*>(dlsym(RTLD_DEFAULT, name));
  if (!original || !wrapper || (*wrapper & 0xFC000000u) != 0x14000000u) return nullptr;
  auto* tramp = static_cast<uint32_t*>(AllocateNear(reinterpret_cast<uintptr_t>(wrapper)));
  if (!tramp) return nullptr;
  tramp[0] = 0x58000050;  // ldr x16, #8
  tramp[1] = 0xD61F0200;  // br x16
  std::memcpy(tramp + 2, &hook, sizeof(hook));
  mach_vm_protect(mach_task_self(), mach_vm_address_t(tramp), vm_page_size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
  sys_icache_invalidate(tramp, 16);
  const mach_vm_address_t page = reinterpret_cast<uintptr_t>(wrapper) & ~(uintptr_t(vm_page_size) - 1);
  if (mach_vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) !=
      KERN_SUCCESS) {
    return nullptr;
  }
  const intptr_t delta = (reinterpret_cast<intptr_t>(tramp) - reinterpret_cast<intptr_t>(wrapper)) / 4;
  *wrapper = 0x14000000u | (uint32_t(delta) & 0x03FFFFFFu);  // b tramp
  mach_vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
  sys_icache_invalidate(wrapper, 4);
  return original;
}

// ---------------------------------------------------------------- setup
// The address other players reach us at: the interface that routes to the server,
// or (server on this Mac) the first active non-loopback IPv4 interface.
uint32_t LocalAddress() {
  if (const char* env = std::getenv("SKATE3_LAN_IP")) {
    in_addr a{};
    if (inet_pton(AF_INET, env, &a) == 1) return ntohl(a.s_addr);
  }
  if ((g_server >> 24) != 127) {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(kBlazePort);
    to.sin_addr.s_addr = htonl(g_server);
    sockaddr_in me{};
    socklen_t len = sizeof(me);
    const bool ok = fd >= 0 && connect(fd, (sockaddr*)&to, sizeof(to)) == 0 && getsockname(fd, (sockaddr*)&me, &len) == 0;
    if (fd >= 0) close(fd);
    if (ok && me.sin_addr.s_addr) return ntohl(me.sin_addr.s_addr);
  }
  uint32_t found = 0x7F000001;
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) == 0) {
    for (ifaddrs* i = list; i; i = i->ifa_next) {
      if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK))
        continue;
      found = ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr);
      if (std::strcmp(i->ifa_name, "en0") == 0) break;
    }
    freeifaddrs(list);
  }
  return found;
}

std::filesystem::path SupportDir() {
  const char* home = std::getenv("HOME");
  return std::filesystem::path(home ? home : "/tmp") / "Library/Application Support/Skate3 LAN";
}

__attribute__((constructor)) void Init() {
  std::error_code ec;
  std::filesystem::create_directories(SupportDir(), ec);
  const char* log_path = std::getenv("SKATE3_LAN_LOG");
  g_log = std::fopen(log_path ? log_path : (SupportDir() / "lan.log").c_str(), "w");
  if (const char* env = std::getenv("SKATE3_LAN_SERVER")) {
    in_addr a{};
    if (inet_pton(AF_INET, env, &a) == 1) g_server = ntohl(a.s_addr);
  }
  std::snprintf(g_server_text, sizeof(g_server_text), "%s", IpText(g_server).c_str());
  if (const char* env = std::getenv("SKATE3_LAN_NAME")) g_name = std::string(env).substr(0, 15);
  if (const char* env = std::getenv("SKATE3_LAN_XUID")) g_xuid = std::strtoull(env, nullptr, 16);
  g_local = LocalAddress();
  // Writing to a connection the other side closed raises SIGPIPE on macOS, which
  // kills the game silently (exit 141); on the console send() just fails.
  signal(SIGPIPE, SIG_IGN);
  g_bind_local = std::getenv("SKATE3_LAN_IP") != nullptr;
  std::thread(LockWatchdog).detach();
  std::thread(TrafficLog).detach();
  if (const char* input = std::getenv("SKATE3_LAN_INPUT")) std::thread(InputScript, std::string(input)).detach();
  InstallCrashHandler();
  Log("Skate 3 LAN loaded (pid %d): server %s, this Mac %s, name \"%s\", XUID %016llX", getpid(), g_server_text,
      IpText(g_local).c_str(), g_name.c_str(), static_cast<unsigned long long>(g_xuid));
  g_blaze_init = HookDirect("sub_829953A0", BlazeInit);
  Log("BlazeHub hook %s", g_blaze_init ? "ready" : "MISSING: the game will still try EA's servers");
  g_netconn_startup = HookDirect("sub_82A1B8E0", NetConnStartup);
  Log("NetConn hook %s", g_netconn_startup ? "ready" : "MISSING: online stays on 'EA server not available'");
  g_blaze_print = HookDirect("sub_829967F8", BlazePrint);
  Log("BlazeSDK log hook %s", g_blaze_print ? "ready" : "missing");
  g_cana_add_game = HookDirect("sub_829DFED0", CanaAddGame);
  g_cana_add_player = HookDirect("sub_829DFFE0", CanaAddPlayer);
  g_cana_connect_player = HookDirect("sub_829DFC88", CanaConnectPlayer);
  g_connapi_host = HookDirect("sub_82A23E98", ConnApiHost);
  g_connapi_session = HookDirect("sub_82A23538", ConnApiSession);
  g_session_control = HookDirect("sub_82A2D928", SessionControl);
  g_session_other = HookDirect("sub_82A3B150", SessionOther);
  g_get_xuid = HookDirect("sub_82EDE580", GetXuid);
  Log("XUID hook %s", g_get_xuid ? "ready" : "MISSING: every default profile logs in with the same XUID");
  g_netconn_user = HookDirect("sub_82A1AED8", NetConnUser);
  Log("NetConn user hook %s", g_netconn_user ? "ready" : "MISSING: Xbox session creation will fail");
  g_netconn_status = HookDirect("sub_82A1C080", NetConnStatus);
  g_netconn_update = HookDirect("sub_82A1B310", NetConnUpdate);
  g_blaze_connect = HookDirect("sub_82994560", BlazeConnect);
  g_friends_enum = HookDirect("sub_82F395E0", FriendsCreateEnumerator);
  Log("Xbox friends hook %s", g_friends_enum ? "ready" : "MISSING: the friends menu stays empty");
  g_language_relocate = HookDirect("sub_8290D638", LanguageRelocateHook);
  Log("menu names hook %s", g_language_relocate ? "ready" : "MISSING: menus keep Xbox LIVE");
  g_invite_info = HookDirect("sub_82F39740", InviteAcceptedInfo);
  Log("invite hook %s", g_invite_info ? "ready" : "MISSING: invites can't be accepted");
  std::thread(FriendsPoller).detach();
  std::thread(InstallTitleLogo).detach();
  g_suboption_count = HookDirect("sub_8261DB48", SubOptionCount);
  std::thread(TrimOnlineMenu).detach();
  Log("diagnostic hooks: %s %s %s", g_netconn_status ? "status" : "-", g_netconn_update ? "update" : "-",
      g_blaze_connect ? "connect" : "-");
}

struct Interpose {
  const void* replacement;
  const void* original;
};
#define INTERPOSE(fn, name) {reinterpret_cast<const void*>(&fn), reinterpret_cast<const void*>(&Original_##name)}
__attribute__((used, section("__DATA,__interpose"))) const Interpose kInterposes[] = {
    INTERPOSE(SigninState, XamUserGetSigninState),
    INTERPOSE(SigninInfo, XamUserGetSigninInfo),
    INTERPOSE(CheckPrivilege, XamUserCheckPrivilege),
    INTERPOSE(UserName, XamUserGetName),
    INTERPOSE(AreFriends, XamUserAreUsersFriends),
    INTERPOSE(ShowGameInviteUI, XamShowGameInviteUI),
    INTERPOSE(XNetStartup, NetDll_XNetStartup),
    INTERPOSE(LinkStatus, NetDll_XNetGetEthernetLinkStatus),
    INTERPOSE(TitleXnAddr, NetDll_XNetGetTitleXnAddr),
    INTERPOSE(XnAddrToInAddr, NetDll_XNetXnAddrToInAddr),
    INTERPOSE(InAddrToXnAddr, NetDll_XNetInAddrToXnAddr),
    INTERPOSE(ServerToInAddr, NetDll_XNetServerToInAddr),
    INTERPOSE(Succeed, NetDll_XNetConnect),
    INTERPOSE(ConnectStatus, NetDll_XNetGetConnectStatus),
    INTERPOSE(CreateKey, NetDll_XNetCreateKey),
    INTERPOSE(Succeed, NetDll_XNetRegisterKey),
    INTERPOSE(Succeed, NetDll_XNetUnregisterKey),
    INTERPOSE(QosLookup, NetDll_XNetQosLookup),
    INTERPOSE(Succeed, NetDll_XNetQosListen),
    INTERPOSE(DnsLookup, NetDll_XNetDnsLookup),
    INTERPOSE(Connect, NetDll_connect),
    INTERPOSE(Bind, NetDll_bind),
    INTERPOSE(Accept, NetDll_accept),
    INTERPOSE(Recv, NetDll_recv),
    INTERPOSE(RecvFrom, NetDll_recvfrom),
    INTERPOSE(Send, NetDll_send),
    INTERPOSE(SendTo, NetDll_sendto),
    INTERPOSE(Select, NetDll_select),
    INTERPOSE(IoctlSocket, NetDll_ioctlsocket),
    INTERPOSE(SetSockOpt, NetDll_setsockopt),
    INTERPOSE(GetSockOpt, NetDll_getsockopt),
    INTERPOSE(GetSockName, NetDll_getsockname),
    INTERPOSE(GetPeerName, NetDll_getpeername),
    INTERPOSE(CloseSocket, NetDll_closesocket),
    INTERPOSE(WsaRecvFrom, NetDll_WSARecvFrom),
    INTERPOSE(WsaRecv, NetDll_WSARecv),
    INTERPOSE(WsaGetOverlappedResult, NetDll_WSAGetOverlappedResult),
    INTERPOSE(TerminateTitle, XamLoaderTerminateTitle),
    INTERPOSE(XMsgStart, XMsgStartIORequest),
    INTERPOSE(XMsgStartEx, XMsgStartIORequestEx),
    INTERPOSE(XMsgCall, XMsgInProcessCall),
    INTERPOSE(LaunchTitle, XamLoaderLaunchTitle),
    {reinterpret_cast<const void*>(&ExitHook), reinterpret_cast<const void*>(&::exit)},
    {reinterpret_cast<const void*>(&UnderscoreExitHook), reinterpret_cast<const void*>(&::_exit)},
    {reinterpret_cast<const void*>(&UnderscoreExitHook), reinterpret_cast<const void*>(&::_Exit)},
    {reinterpret_cast<const void*>(&AbortHook), reinterpret_cast<const void*>(&::abort)},
};

}  // namespace
