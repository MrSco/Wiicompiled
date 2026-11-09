#include "voice/voice_network.h"

#include "voice/voice_packet.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace Voice {
namespace {

#ifdef _WIN32
using VoiceSocket = SOCKET;
constexpr VoiceSocket kNoSocket = INVALID_SOCKET;
#else
using VoiceSocket = int;
constexpr VoiceSocket kNoSocket = -1;
#endif

using LinkClock = std::chrono::steady_clock;

// Unanswered members are greeted twice a second; answered ones every two
// seconds, which also keeps both routers' mappings open.
constexpr auto kHelloInterval = std::chrono::milliseconds(500);
constexpr auto kKeepAliveInterval = std::chrono::seconds(2);
// A member that has sent nothing for this long is treated as gone.
constexpr auto kReachTimeout = std::chrono::seconds(6);
constexpr int kPollMilliseconds = 20;

void CloseVoiceSocket(VoiceSocket socket) {
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

bool WaitReadable(VoiceSocket socket, int timeoutMs) {
#ifdef _WIN32
    WSAPOLLFD fd{};
    fd.fd = socket;
    fd.events = POLLRDNORM;
    return WSAPoll(&fd, 1, timeoutMs) > 0;
#else
    pollfd fd{};
    fd.fd = socket;
    fd.events = POLLIN;
    return poll(&fd, 1, timeoutMs) > 0;
#endif
}

void MakeNonBlocking(VoiceSocket socket) {
#ifdef _WIN32
    u_long enabled = 1;
    ioctlsocket(socket, FIONBIO, &enabled);
    // Without this, an ICMP port-unreachable from a member without voice chat
    // makes the next recvfrom fail with WSAECONNRESET.
    BOOL disabled = FALSE;
    DWORD bytes = 0;
    WSAIoctl(socket, SIO_UDP_CONNRESET, &disabled, sizeof(disabled), nullptr, 0, &bytes, nullptr, nullptr);
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    fcntl(socket, F_SETFL, flags | O_NONBLOCK);
#endif
}

uint16_t AddToPort(uint16_t portNetworkOrder, int delta) {
    return htons(static_cast<uint16_t>(ntohs(portNetworkOrder) + delta));
}

}  // namespace

struct Link::Impl {
    struct PeerState {
        LinkPeer info;
        bool reached = false;
        uint16_t voicePort = 0;  // network order; where this member's voice packets come from
        LinkClock::time_point lastHeard{};
        LinkClock::time_point lastHello{};
    };

    explicit Impl(Deliver deliverFn) : deliver(std::move(deliverFn)) {}

    struct Received {
        uint64_t speakerId = 0;
        uint16_t sequence = 0;
        std::vector<uint8_t> frame;
    };

    Deliver deliver;
    // Guards everything below. Received frames are handed to `deliver` only
    // after it is released: the engine sends while holding its own lock, so
    // calling into the engine under this one could deadlock.
    mutable std::mutex mutex;
    std::vector<Received> received;
    VoiceSocket socket = kNoSocket;
    uint16_t boundPort = 0;
    bool winsockStarted = false;
    std::thread worker;
    std::atomic<bool> stop{false};
    uint32_t localProfileId = 0;
    uint16_t helloSequence = 0;
    std::map<uint64_t, PeerState> peers;
    // For LinkStats: everyone seen and reached since Open, by speaker id.
    std::set<uint64_t> membersSeen;
    std::set<uint64_t> membersReached;
    LinkStats stats;
    const LinkClock::time_point epoch = LinkClock::now();

    uint32_t NowMillis() const {
        return static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(LinkClock::now() - epoch).count());
    }

    void SendTo(uint32_t ip, uint16_t port, const std::vector<uint8_t>& packet) const {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = ip;
        to.sin_port = port;
        sendto(socket, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
               reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    }

    // Finds the member a packet came from: same address, and when the game
    // knows that member's profile, the same profile id. Two consoles behind
    // one router share an address and are told apart by profile id.
    PeerState* Sender(const sockaddr_in& from, uint32_t senderProfileId) {
        PeerState* unidentified = nullptr;
        for (auto& [id, peer] : peers) {
            if (peer.info.ip != from.sin_addr.s_addr) {
                continue;
            }
            if (peer.info.profileId == senderProfileId && senderProfileId != 0) {
                return &peer;
            }
            if (peer.info.profileId == 0 && !unidentified) {
                unidentified = &peer;
            }
        }
        return unidentified;
    }

    void HandlePacket(const uint8_t* data, size_t size, const sockaddr_in& from) {
        const std::optional<PacketView> packet = DecodePacket(data, size);
        if (!packet) {
            return;
        }
        PeerState* peer = Sender(from, packet->senderProfileId);
        if (!peer) {
            return;  // not someone the game is racing with
        }
        const LinkClock::time_point now = LinkClock::now();
        if (packet->type == PacketType::Bye) {
            if (peer->reached) {
                std::fprintf(stderr, "[audio] voice chat: profile %u left voice chat\n", peer->info.profileId);
            }
            peer->reached = false;
            return;
        }
        if (!peer->reached) {
            std::fprintf(stderr, "[audio] voice chat: connected to profile %u\n", peer->info.profileId);
        }
        peer->reached = true;
        membersReached.insert(peer->info.speakerId);
        peer->voicePort = from.sin_port;
        peer->lastHeard = now;
        switch (packet->type) {
        case PacketType::Hello:
            SendTo(from.sin_addr.s_addr, from.sin_port,
                   EncodeClockPacket(PacketType::HelloAck, packet->sequence, localProfileId, ReadClock(*packet)));
            break;
        case PacketType::Voice:
            ++stats.framesReceived;
            received.push_back({peer->info.speakerId, packet->sequence,
                                std::vector<uint8_t>(packet->payload, packet->payload + packet->payloadSize)});
            break;
        default:
            break;
        }
    }

    void SendHellos() {
        const LinkClock::time_point now = LinkClock::now();
        for (auto& [id, peer] : peers) {
            if (peer.reached && now - peer.lastHeard > kReachTimeout) {
                std::fprintf(stderr, "[audio] voice chat: lost profile %u\n", peer.info.profileId);
                peer.reached = false;
            }
            const auto interval = peer.reached ? kKeepAliveInterval : kHelloInterval;
            if (now - peer.lastHello < interval) {
                continue;
            }
            peer.lastHello = now;
            const auto hello = EncodeClockPacket(PacketType::Hello, helloSequence++, localProfileId, NowMillis());
            if (peer.reached) {
                SendTo(peer.info.ip, peer.voicePort, hello);
                continue;
            }
            // Predicted voice port: the member's game port + 1.
            SendTo(peer.info.ip, AddToPort(peer.info.gamePort, 1), hello);
            if (peer.voicePort != 0 && peer.voicePort != AddToPort(peer.info.gamePort, 1)) {
                SendTo(peer.info.ip, peer.voicePort, hello);  // where they last answered from
            }
        }
    }

    void Run() {
        std::array<uint8_t, 1500> buffer{};
        std::vector<Received> ready;
        while (!stop) {
            WaitReadable(socket, kPollMilliseconds);
            {
                std::lock_guard<std::mutex> lock(mutex);
                ReceiveAndGreet(buffer);
                ready.swap(received);
            }
            for (const Received& frame : ready) {
                if (deliver) {
                    deliver(frame.speakerId, frame.sequence, frame.frame.data(), frame.frame.size());
                }
            }
            ready.clear();
        }
    }

    // Under `mutex`.
    void ReceiveAndGreet(std::array<uint8_t, 1500>& buffer) {
        for (;;) {
            sockaddr_in from{};
            socklen_t fromLength = sizeof(from);
            const int got = recvfrom(socket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()),
                                     0, reinterpret_cast<sockaddr*>(&from), &fromLength);
            if (got < 0) {
                break;
            }
            HandlePacket(buffer.data(), static_cast<size_t>(got), from);
        }
        SendHellos();
    }
};

Link::Link(Deliver deliver) : m_impl(std::make_unique<Impl>(std::move(deliver))) {}

Link::~Link() {
    Close();
}

bool Link::Open(uint16_t localGamePort, uint32_t bindAddress) {
    Close();
    Impl& impl = *m_impl;
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return false;
    }
    impl.winsockStarted = true;
#endif
    impl.socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl.socket == kNoSocket) {
        Close();
        return false;
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = bindAddress;
    local.sin_port = localGamePort != 0 ? htons(static_cast<uint16_t>(localGamePort + 1)) : 0;
    if (bind(impl.socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        // Port prediction is lost, but members can still reach us through the
        // replies to our own Hellos.
        local.sin_port = 0;
        if (bind(impl.socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
            Close();
            return false;
        }
    }
    socklen_t length = sizeof(local);
    getsockname(impl.socket, reinterpret_cast<sockaddr*>(&local), &length);
    impl.boundPort = ntohs(local.sin_port);
    impl.membersSeen.clear();
    impl.membersReached.clear();
    impl.stats = {};
    MakeNonBlocking(impl.socket);
    std::fprintf(stderr, "[audio] voice chat: voice socket on port %u (game port %u)\n", impl.boundPort,
                 localGamePort);
    impl.stop = false;
    impl.worker = std::thread([&impl] { impl.Run(); });
    return true;
}

void Link::Close() {
    Impl& impl = *m_impl;
    if (impl.worker.joinable()) {
        impl.stop = true;
        impl.worker.join();
    }
    std::lock_guard<std::mutex> lock(impl.mutex);
    if (impl.socket != kNoSocket) {
        impl.stats.membersSeen = impl.membersSeen.size();
        impl.stats.membersReached = impl.membersReached.size();
        const LinkStats& stats = impl.stats;
        std::fprintf(stderr,
                     "[audio] voice chat: left room: voice chat reached %zu of %zu player(s); "
                     "sent %llu of %llu frame(s) you spoke, received %llu frame(s)\n",
                     stats.membersReached, stats.membersSeen, static_cast<unsigned long long>(stats.framesSent),
                     static_cast<unsigned long long>(stats.framesSpoken),
                     static_cast<unsigned long long>(stats.framesReceived));
        const auto bye = EncodePacket(PacketType::Bye, 0, impl.localProfileId, nullptr, 0);
        for (const auto& [id, peer] : impl.peers) {
            if (peer.reached) {
                impl.SendTo(peer.info.ip, peer.voicePort, bye);
            }
        }
        CloseVoiceSocket(impl.socket);
        impl.socket = kNoSocket;
    }
    impl.peers.clear();
    impl.received.clear();
    impl.boundPort = 0;
#ifdef _WIN32
    if (impl.winsockStarted) {
        WSACleanup();
        impl.winsockStarted = false;
    }
#endif
}

bool Link::IsOpen() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->socket != kNoSocket;
}

uint16_t Link::BoundPort() const {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->boundPort;
}

void Link::SetRoom(uint32_t localProfileId, const std::vector<LinkPeer>& peers) {
    Impl& impl = *m_impl;
    std::lock_guard<std::mutex> lock(impl.mutex);
    impl.localProfileId = localProfileId;
    std::map<uint64_t, Impl::PeerState> next;
    for (const LinkPeer& peer : peers) {
        if (peer.ip == 0 || peer.gamePort == 0) {
            continue;
        }
        auto existing = impl.peers.find(peer.speakerId);
        Impl::PeerState state = existing != impl.peers.end() ? existing->second : Impl::PeerState{};
        state.info = peer;
        next[peer.speakerId] = state;
        impl.membersSeen.insert(peer.speakerId);
    }
    impl.peers.swap(next);
}

void Link::SendVoice(uint16_t sequence, const uint8_t* data, size_t size) {
    Impl& impl = *m_impl;
    std::lock_guard<std::mutex> lock(impl.mutex);
    if (impl.socket == kNoSocket || size == 0 || size > kMaxPacketBytes) {
        return;
    }
    ++impl.stats.framesSpoken;
    const auto packet = EncodePacket(PacketType::Voice, sequence, impl.localProfileId, data, size);
    bool sent = false;
    for (const auto& [id, peer] : impl.peers) {
        if (peer.reached) {
            impl.SendTo(peer.info.ip, peer.voicePort, packet);
            sent = true;
        }
    }
    if (sent) {
        ++impl.stats.framesSent;
    }
}

LinkStats Link::Stats() const {
    const Impl& impl = *m_impl;
    std::lock_guard<std::mutex> lock(impl.mutex);
    LinkStats stats = impl.stats;
    stats.membersSeen = impl.membersSeen.size();
    stats.membersReached = impl.membersReached.size();
    return stats;
}

bool Link::Reached(uint64_t speakerId) const {
    const Impl& impl = *m_impl;
    std::lock_guard<std::mutex> lock(impl.mutex);
    const auto it = impl.peers.find(speakerId);
    return it != impl.peers.end() && it->second.reached;
}

}  // namespace Voice
