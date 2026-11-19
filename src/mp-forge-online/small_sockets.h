// Sockets for MP Forge's own code that goes online: the radio it gives to plugins, and the test cards.
//
// SPDX-License-Identifier: MIT
//
// Everything here is meant for a thread of the plugin's own, never for the audio thread. Nothing waits for
// long: a socket is asked whether it is ready a tenth of a second at a time, and gives up at once when the
// plugin says it is leaving, so removing a plugin never holds the MPC up.

#pragma once

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mp_forge_net {

constexpr int kNoSocket       = -1;
constexpr int kLookEveryMs    = 100;
constexpr int kLongestWaitMs  = 3000;
constexpr int kNameServerPort = 53;
// A public name server, asked directly, so a name is looked up without anything that could wait unseen.
constexpr const char* kNameServer = "1.1.1.1";

inline int socketThatDoesNotWait(int kind)
{
    const int made = socket(AF_INET, kind, 0);
    if (made != kNoSocket)
        fcntl(made, F_SETFL, fcntl(made, F_GETFL, 0) | O_NONBLOCK);
    return made;
}

// Waits until the socket can be read from or written to. False when that took too long or the plugin is leaving.
inline bool ready(int socket, short wanted, const std::atomic<bool>& leaving, int longestWaitMs = kLongestWaitMs)
{
    for (int waited = 0; waited < longestWaitMs && !leaving.load(std::memory_order_relaxed); waited += kLookEveryMs)
    {
        pollfd watched { socket, wanted, 0 };
        if (poll(&watched, 1, kLookEveryMs) > 0)
            return (watched.revents & wanted) != 0;
    }
    return false;
}

inline sockaddr_in at(in_addr address, int port)
{
    sockaddr_in where {};
    where.sin_family = AF_INET;
    where.sin_port   = htons((uint16_t)port);
    where.sin_addr   = address;
    return where;
}

// A send that cannot stop the whole app when the far end has gone.
inline bool sent(int socket, const void* bytes, size_t count)
{
#ifdef MSG_NOSIGNAL
    const int quietly = MSG_NOSIGNAL;
#else
    const int quietly = 0;
    const int on = 1;
    setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    return send(socket, bytes, count, quietly) == (ssize_t)count;
}

// A name is either pieces that end in a zero, or two bytes that point back at one written earlier.
inline size_t pastTheName(const unsigned char* answer, size_t length, size_t at)
{
    while (at < length)
    {
        const unsigned char piece = answer[at];
        if ((piece & 0xC0) == 0xC0)
            return at + 2;
        if (piece == 0)
            return at + 1;
        at += (size_t)piece + 1;
    }
    return length;
}

// Reads the first address out of a name server's answer. The answers start where the question ended.
inline bool addressIn(const unsigned char* answer, size_t length, size_t answersStart, in_addr& address)
{
    size_t at = answersStart;
    while (at < length)
    {
        at = pastTheName(answer, length, at);
        if (at + 10 > length)
            return false;
        const int    kind       = (answer[at] << 8) | answer[at + 1];
        const size_t dataLength = (size_t)((answer[at + 8] << 8) | answer[at + 9]);
        at += 10;
        if (at + dataLength > length)
            return false;
        if (kind == 1 && dataLength == sizeof(address.s_addr))
        {
            std::memcpy(&address.s_addr, answer + at, dataLength);
            return true;
        }
        at += dataLength;
    }
    return false;
}

// Writes a name server's question about a name: an id, "please look further for me", one question, the name in
// pieces, then "its address" and "on the internet". Returns its length, or 0 for a name too long to ask about.
inline size_t questionAbout(const char* name, unsigned char* question, size_t room)
{
    static const unsigned char opening[] = { 0x4d, 0x50, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const unsigned char closing[] = { 0x00, 0x01, 0x00, 0x01 };
    const size_t nameLength = std::strlen(name);
    if (sizeof(opening) + nameLength + 2 + sizeof(closing) > room)
        return 0;
    std::memcpy(question, opening, sizeof(opening));
    size_t at = sizeof(opening);
    for (const char* piece = name; *piece != 0;)
    {
        const char*  dot    = std::strchr(piece, '.');
        const size_t length = dot != nullptr ? (size_t)(dot - piece) : std::strlen(piece);
        question[at++] = (unsigned char)length;
        std::memcpy(question + at, piece, length);
        at += length;
        piece += length + (dot != nullptr ? 1 : 0);
    }
    question[at++] = 0;
    std::memcpy(question + at, closing, sizeof(closing));
    return at + sizeof(closing);
}

// The address of a name, asked of the public name server. False when no answer came in time.
inline bool addressOf(const char* name, const std::atomic<bool>& leaving, in_addr& address)
{
    unsigned char question[300];
    const size_t  asked  = questionAbout(name, question, sizeof(question));
    const int     asking = socketThatDoesNotWait(SOCK_DGRAM);
    if (asked == 0 || asking == kNoSocket)
        return false;
    in_addr nameServer {};
    inet_pton(AF_INET, kNameServer, &nameServer);
    const sockaddr_in where = at(nameServer, kNameServerPort);
    unsigned char answer[512];
    ssize_t length = 0;
    if (connect(asking, (const sockaddr*)&where, sizeof(where)) == 0 && sent(asking, question, asked) && ready(asking, POLLIN, leaving))
        length = recv(asking, answer, sizeof(answer), 0);
    close(asking);
    return length > 0 && addressIn(answer, (size_t)length, asked, address);
}

// Whether an address belongs to this computer or to a network of the musician's own, where nothing fetched for a
// plugin has any business: 0.x, 10.x, 127.x, 169.254.x, 172.16 to 172.31, and 192.168.x.
inline bool isOnALocalNetwork(in_addr address)
{
    const uint32_t number = ntohl(address.s_addr);
    const uint32_t first  = number >> 24;
    const uint32_t second = (number >> 16) & 0xff;
    const bool     own    = first == 0 || first == 10 || first == 127;
    return own || (first == 169 && second == 254) || (first == 172 && second >= 16 && second <= 31) || (first == 192 && second == 168);
}

inline bool nothingWentWrongOn(int socket)
{
    int       wrong  = 0;
    socklen_t length = sizeof(wrong);
    return getsockopt(socket, SOL_SOCKET, SO_ERROR, &wrong, &length) == 0 && wrong == 0;
}

// A connection to the address, or kNoSocket when none was made in time.
inline int connectedTo(in_addr address, int port, const std::atomic<bool>& leaving)
{
    const int made = socketThatDoesNotWait(SOCK_STREAM);
    if (made == kNoSocket)
        return kNoSocket;
    const sockaddr_in where   = at(address, port);
    const bool        started = connect(made, (const sockaddr*)&where, sizeof(where)) == 0 || errno == EINPROGRESS;
    if (started && ready(made, POLLOUT, leaving) && nothingWentWrongOn(made))
        return made;
    close(made);
    return kNoSocket;
}

// The three digits a web server puts after the first word of its answer, such as "HTTP/1.1 200 OK". 0 when there are none.
inline int codeIn(const char* firstLine)
{
    const char* afterTheSpace = std::strchr(firstLine, ' ');
    return afterTheSpace != nullptr ? std::atoi(afterTheSpace + 1) : 0;
}

} // namespace mp_forge_net
