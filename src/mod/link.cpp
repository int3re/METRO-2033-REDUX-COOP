#include "mod/link.h"

#include <windows.h>

#include <cstring>
#include <mutex>
#include <vector>

#include "common/log.h"
#include "common/version.h"
#include "mod/game.h"
#include "mod/patch.h"

namespace link {
namespace {

// ENet 1.3 as it is built into the exe (the fields this code reads).
constexpr size_t kHostPeers = 40;       // ENetHost + : ENetPeer* peers
constexpr size_t kHostPeerCount = 48;   // ENetHost + : size_t peerCount
constexpr size_t kPeerSize = 448;
constexpr size_t kPeerAddress = 36;     // ENetPeer + : ENetAddress {u32 host; u16 port} (exe+8ED711 hands peer + 0x24 to the socket send)
constexpr size_t kPeerState = 56;       // ENetPeer + : int state
constexpr int kPeerConnected = 5;
constexpr int kEventReceive = 3;
struct Event {       // ENetEvent
  int type;
  uint8_t* peer;
  uint8_t channel;
  uint32_t data;
  uint8_t* packet;
};
constexpr size_t kPacketFlags = 8;      // ENetPacket + : u32 flags (1 reliable, 4 the data is not the packet's)
constexpr size_t kPacketData = 16;
constexpr size_t kPacketSize = 24;
constexpr size_t kPacketFree = 32;      // ENetPacket + : void (*freeCallback)(ENetPacket*)

constexpr uint8_t kMark = 0xFE;         // the engine's message types are 0..13
constexpr size_t kHead = 4;             // FE 'M' 'N' type
constexpr size_t kNetCoreLock = 0x188;  // net core + : the critical section the engine holds around ENet

struct Message {
  uint8_t* peer;
  uint8_t type;
  std::vector<uint8_t> data;
};
std::mutex g_lock;
std::vector<Message> g_inbox;
Handler g_handlers[256] = {};
std::vector<uint8_t*> g_greeted;  // the peers a hello went to
uint32_t g_connections = 0;

uint8_t* enet_host() {
  uintptr_t h = game::ref<uintptr_t>(game::kNetHost);
  return h > 1 ? reinterpret_cast<uint8_t*>(h) : nullptr;
}

void free_packet(uint8_t* packet) {
  using Free = void (*)(void*);
  Free release = game::ref<Free>(game::kEnetFree);
  auto callback = *reinterpret_cast<void (**)(uint8_t*)>(packet + kPacketFree);
  if (callback) callback(packet);
  if (!(*reinterpret_cast<uint32_t*>(packet + kPacketFlags) & 4)) release(*reinterpret_cast<void**>(packet + kPacketData));
  release(packet);
}

// The engine asks ENet for the next event; a packet of the mod's own never reaches it.
using ServiceFn = int (*)(uint8_t*, Event*, uint32_t);
ServiceFn g_service = nullptr;

int on_service(uint8_t* host, Event* event, uint32_t timeout) {
  for (;;) {
    int got = g_service(host, event, timeout);
    if (!event) return got;
    if (got <= 0 || event->type != kEventReceive || !event->packet) return got;
    const uint8_t* data = *reinterpret_cast<uint8_t**>(event->packet + kPacketData);
    size_t size = *reinterpret_cast<size_t*>(event->packet + kPacketSize);
    if (size < kHead || !data || data[0] != kMark || data[1] != 'M' || data[2] != 'N') return got;
    {
      std::lock_guard<std::mutex> hold(g_lock);
      if (g_inbox.size() < 4096) g_inbox.push_back({event->peer, data[3], std::vector<uint8_t>(data + kHead, data + size)});
    }
    free_packet(event->packet);
    event->packet = nullptr;
  }
}

bool connected(const uint8_t* peer) { return *reinterpret_cast<const int*>(peer + kPeerState) == kPeerConnected; }

template <typename Fn>
void each_peer(Fn fn) {
  uint8_t* host = enet_host();
  if (!host) return;
  uint8_t* peers = *reinterpret_cast<uint8_t**>(host + kHostPeers);
  size_t count = *reinterpret_cast<size_t*>(host + kHostPeerCount);
  if (!peers || count > 4096) return;
  for (size_t i = 0; i < count; ++i)
    if (connected(peers + i * kPeerSize)) fn(peers + i * kPeerSize);
}

struct Players {
  uint8_t* array;
  uint16_t capacity;
  uint16_t size;
};

uint8_t* peer_of_slot(int slot) {
  Players& p = game::ref<Players>(game::kNetPlayers);
  if (slot < 0 || slot >= p.size || !p.array) return nullptr;
  return *reinterpret_cast<uint8_t**>(p.array + game::kNetPlayerSize * slot + game::kNetPlayerPeer);
}

int slot_of_peer(const uint8_t* peer) {
  if (game::ref<int>(game::kNetLocalPlayer) != 0) return 0;  // a joined game hears the host only
  Players& p = game::ref<Players>(game::kNetPlayers);
  for (int i = 0; i < p.size && p.array; ++i)
    if (*reinterpret_cast<uint8_t**>(p.array + game::kNetPlayerSize * i + game::kNetPlayerPeer) == peer) return i;
  return -1;
}

bool send_to(uint8_t* peer, Type type, const void* data, size_t size, bool reliable) {
  uint8_t stack[512];
  std::vector<uint8_t> heap;
  uint8_t* buffer = stack;
  if (size + 3 > sizeof(stack)) {
    heap.resize(size + 3);
    buffer = heap.data();
  }
  buffer[0] = 'M', buffer[1] = 'N', buffer[2] = type;
  if (size) memcpy(buffer + 3, data, size);
  using Create = uint8_t* (*)(char, const void*, size_t, int);  // the engine's: one byte in front of the data
  using Send = int (*)(uint8_t*, uint8_t, uint8_t*);
  uint8_t* packet = reinterpret_cast<Create>(game::at(game::kEnetPacketCreate))(static_cast<char>(kMark), buffer, size + 3, reliable ? 1 : 0);
  if (!packet) return false;
  if (reinterpret_cast<Send>(game::at(game::kEnetPeerSend))(peer, 0, packet) < 0) {
    if (*reinterpret_cast<size_t*>(packet) == 0) free_packet(packet);  // nobody took it
    return false;
  }
  return true;
}

struct Hello {
  uint16_t protocol;
  char version[16];
};

void on_hello(int from, const uint8_t* data, size_t size) {
  if (size < sizeof(Hello)) return;
  Hello h;
  memcpy(&h, data, sizeof(h));
  h.version[sizeof(h.version) - 1] = 0;
  if (h.protocol == kProtocol)
    LOGI("link: player %d runs the mod %s (the same protocol %u)", from, h.version, h.protocol);
  else
    LOGE("link: player %d runs the mod %s with protocol %u, this one is %s with %u: the games will not agree - both need the same version",
         from, h.version, h.protocol, METRONET_VERSION, kProtocol);
}

}  // namespace

void install() {
  patch::hook("ENet service", game::at(game::kEnetService), reinterpret_cast<void*>(&on_service), reinterpret_cast<void**>(&g_service));
  on(kHello, &on_hello);
}

void on(Type type, Handler fn) { g_handlers[type] = fn; }

uint32_t connections() { return g_connections; }

uint32_t address_of_slot(int slot) {
  uint8_t* peer = enet_host() ? peer_of_slot(slot) : nullptr;
  return peer ? *reinterpret_cast<uint32_t*>(peer + kPeerAddress) : 0;
}

int peers() {
  int n = 0;
  each_peer([&](uint8_t*) { ++n; });
  return n;
}

bool send(Type type, const void* data, size_t size, bool reliable, int to_slot) {
  uint8_t* core = reinterpret_cast<uint8_t*>(game::at(game::kNetHost));
  if (!enet_host()) return false;
  bool sent = false;
  EnterCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(core + kNetCoreLock));
  if (to_slot >= 0) {
    uint8_t* peer = peer_of_slot(to_slot);
    if (peer && connected(peer)) sent = send_to(peer, type, data, size, reliable);
  } else {
    each_peer([&](uint8_t* peer) { sent |= send_to(peer, type, data, size, reliable); });
  }
  LeaveCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(core + kNetCoreLock));
  return sent;
}

void poll() {
  // a hello to every connection that has not had one
  if (enet_host()) {
    std::vector<uint8_t*> now;
    each_peer([&](uint8_t* peer) { now.push_back(peer); });
    uint8_t* core = reinterpret_cast<uint8_t*>(game::at(game::kNetHost));
    for (uint8_t* peer : now) {
      bool known = false;
      for (uint8_t* g : g_greeted) known |= g == peer;
      if (known) continue;
      Hello h = {kProtocol, {}};
      strncpy(h.version, METRONET_VERSION, sizeof(h.version) - 1);
      EnterCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(core + kNetCoreLock));
      bool ok = send_to(peer, kHello, &h, sizeof(h), true);
      LeaveCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(core + kNetCoreLock));
      LOGI("link: hello to a new connection (%s)", ok ? "sent" : "NOT sent");
      ++g_connections;
    }
    g_greeted.swap(now);
  } else if (!g_greeted.empty()) {
    g_greeted.clear();
  }
  std::vector<Message> inbox;
  {
    std::lock_guard<std::mutex> hold(g_lock);
    inbox.swap(g_inbox);
  }
  for (const Message& m : inbox) {
    Handler fn = g_handlers[m.type];
    if (fn) fn(slot_of_peer(m.peer), m.data.data(), m.data.size());
  }
}

}  // namespace link
