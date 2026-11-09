#pragma once

// Voice chat's network link: one UDP socket of its own, separate from the
// game's GT2 socket, which voice chat never sends on, filters or alters.
//
// Why a separate socket: a non-GT2 datagram arriving on the game socket from a
// connected peer is handed to MKW's RACE packet handler, where WiiLink WFC and
// Wiimmfi kick the sender. Players without voice chat (other WiiCompiled builds,
// Wii, Dolphin) must be able to join and race as if voice chat did not exist.
//
// How peers find each other without any server: the voice socket is bound to
// the game socket's local port + 1, so a router that preserves ports maps it
// to the game's public port + 1. Each side sends Hellos to every room member's
// game address at port + 1; the outgoing Hellos open both routers' mappings.
// A player without voice chat has nothing listening there and simply never
// answers. Hellos are accepted from any port of a member's address, and
// answered where they actually came from, which also covers one side whose
// router does not preserve ports.
//
// Packets are only accepted from the addresses the game itself is racing with,
// and the sender's profile id must match that member.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace Voice {

struct LinkPeer {
    uint64_t speakerId = 0;
    uint32_t profileId = 0;  // 0 = not known yet; any profile at the address is accepted
    uint32_t ip = 0;         // network byte order, the address the game's GT2 socket uses
    uint16_t gamePort = 0;   // network byte order
};

// Counters for one open..close of the link (one room), for the log line
// written when it closes.
struct LinkStats {
    size_t membersSeen = 0;     // room members with an address, over the whole room
    size_t membersReached = 0;  // of those, how many had voice chat answer at some point
    uint64_t framesSpoken = 0;  // frames handed to SendVoice
    uint64_t framesSent = 0;    // of those, how many went to at least one member
    uint64_t framesReceived = 0;
};

class Link {
public:
    // Called on the link's worker thread for each received voice frame.
    using Deliver = std::function<void(uint64_t speakerId, uint16_t sequence, const uint8_t* data, size_t size)>;

    explicit Link(Deliver deliver);
    ~Link();
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    // Binds to localGamePort + 1 (host order; 0 or busy = any free port) and
    // starts the worker. Returns false if no socket could be opened.
    // bindAddress (network order) is all interfaces by default; the tests use
    // loopback.
    bool Open(uint16_t localGamePort, uint32_t bindAddress = 0);
    // Tells reached peers this console left voice chat, then stops.
    void Close();
    bool IsOpen() const;
    uint16_t BoundPort() const;  // host order, 0 when closed

    // Replaces the set of room members to talk to. Members that stay keep
    // their connection state.
    void SetRoom(uint32_t localProfileId, const std::vector<LinkPeer>& peers);
    // Sends one encoded frame to every member whose voice chat answered.
    void SendVoice(uint16_t sequence, const uint8_t* data, size_t size);
    // True while this member's voice chat is answering.
    bool Reached(uint64_t speakerId) const;
    // Since the last Open; kept after Close until the next Open.
    LinkStats Stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace Voice
