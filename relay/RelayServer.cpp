// VRC Downbeat room relay
//
// A tiny standalone TCP relay so hosts don't need a public IP or open ports.
// Both sides connect OUTBOUND to this server:
//
//   Host control:   "HOST <ver> [name]\n"   -> "ROOM <CODE>\n", stays open;
//                                              relay pushes "PEER <token>\n" per joiner
//                                              and "PING\n" keep-alives.
//   Musician:       "JOIN <CODE> [name]\n"  -> "OK\n" (or "ERR <reason>\n"), then waits.
//   Host data:      "DATA <token>\n"        -> paired with the waiting musician;
//                                              from then on both sockets are a
//                                              transparent bidirectional byte pipe
//                                              carrying the normal BandJam protocol.
//
// The optional display names are used for the activity log only - the log
// never contains client IP addresses. (The piped traffic itself is encrypted
// end-to-end by the apps; this relay only ever sees ciphertext.)
//
// Build:  g++ -O2 -std=c++17 -pthread -o bandjam-relay RelayServer.cpp
// Run:    ./bandjam-relay [port] [-v]   (default port 47900)
//
// Logging: lifecycle events (startup, rooms opened/closed) are always
// printed. The ACTIVITY log (per-connection details: who connects, join
// attempts, pairings, transfer statistics) starts enabled with -v and can be
// toggled at runtime without a restart:
//
//     systemctl kill -s USR1 bandjam-relay
//
// Watch everything live with:  journalctl -u bandjam-relay -f

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>

namespace
{
constexpr int kDefaultPort        = 47900;
constexpr int kMaxRooms           = 200;
constexpr int kMaxPendingPerRoom  = 16;
constexpr int kFirstLineTimeoutS  = 10;   // time to send HOST/JOIN/DATA after connecting
constexpr int kPendingTimeoutS    = 30;   // musician waits for the host data connection
constexpr int kPingIntervalS      = 25;   // keep NAT mappings on the control socket alive

struct Pending
{
    int         musicianFd { -1 };
    std::string roomCode;
    std::string musicianName;
    time_t      created { 0 };
};

struct Room
{
    int         hostControlFd { -1 };
    int         numPending    { 0 };
    std::string hostName;
};

std::mutex                     g_lock;
std::map<std::string, Room>    g_rooms;      // code  -> room
std::map<std::string, Pending> g_pending;    // token -> waiting musician
std::mt19937_64                g_rng { std::random_device{}() ^ (uint64_t) ::time (nullptr) };
std::atomic<bool>              g_verbose { false };

void logLine (const std::string& s)
{
    char stamp[32];
    const time_t now = ::time (nullptr);
    std::strftime (stamp, sizeof (stamp), "%Y-%m-%d %H:%M:%S", ::localtime (&now));
    std::printf ("%s  %s\n", stamp, s.c_str());
    std::fflush (stdout);
}

/** Activity log: only printed while verbose mode is on (SIGUSR1 toggles). */
void vlog (const std::string& s)
{
    if (g_verbose.load())
        logLine (s);
}

/** SIGUSR1 handler - only async-signal-safe calls in here. */
void toggleVerbose (int)
{
    const bool now = ! g_verbose.load();
    g_verbose.store (now);
    const char* msg = now ? "activity log: ON\n" : "activity log: OFF\n";
    const ssize_t ignored = ::write (STDOUT_FILENO, msg, std::strlen (msg));
    (void) ignored;
}

/** Display name for the log: printable characters only, capped. Falls back to
    "unknown" so log lines stay readable. IPs are deliberately never logged. */
std::string sanitizeName (const std::string& raw)
{
    std::string s;
    for (const char c : raw)
    {
        if ((unsigned char) c >= 0x20)
            s += c;
        if (s.size() >= 32)
            break;
    }
    while (! s.empty() && s.back() == ' ')
        s.pop_back();
    while (! s.empty() && s.front() == ' ')
        s.erase (s.begin());
    return s.empty() ? "unknown" : s;
}

std::string megabytes (uint64_t bytes)
{
    char buf[32];
    std::snprintf (buf, sizeof (buf), "%.2f MB", (double) bytes / (1024.0 * 1024.0));
    return buf;
}

std::string randomCode (int length, const char* alphabet)
{
    const size_t n = std::strlen (alphabet);
    std::string s;
    for (int i = 0; i < length; ++i)
        s += alphabet[g_rng() % n];
    return s;
}

// Unambiguous alphabet: no 0/O, 1/I/L.
std::string newRoomCode()  { return randomCode (6,  "ABCDEFGHJKMNPQRSTUVWXYZ23456789"); }
std::string newPeerToken() { return randomCode (24, "abcdefghijklmnopqrstuvwxyz0123456789"); }

void setRecvTimeout (int fd, int seconds)
{
    timeval tv {};
    tv.tv_sec = seconds;
    ::setsockopt (fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof (tv));
}

bool sendAll (int fd, const void* data, size_t bytes)
{
    const char* p = static_cast<const char*> (data);
    while (bytes > 0)
    {
        const ssize_t sent = ::send (fd, p, bytes, MSG_NOSIGNAL);
        if (sent <= 0)
            return false;
        p += sent;
        bytes -= (size_t) sent;
    }
    return true;
}

bool sendLine (int fd, const std::string& line)
{
    return sendAll (fd, line.c_str(), line.size());
}

/** Reads one '\n'-terminated line (control phase only, so byte-wise is fine). */
bool readLine (int fd, std::string& out, size_t maxLen = 96)
{
    out.clear();
    char c = 0;
    while (out.size() < maxLen)
    {
        const ssize_t got = ::recv (fd, &c, 1, 0);
        if (got <= 0)
            return false;
        if (c == '\n')
            return true;
        if (c != '\r')
            out += c;
    }
    return false;
}

/** Copies bytes fd -> to until EOF/error, then half-closes the write side.
    Returns the number of bytes forwarded (for the activity log). */
uint64_t pipeOneWay (int from, int to)
{
    char buffer[64 * 1024];
    uint64_t total = 0;
    for (;;)
    {
        const ssize_t got = ::recv (from, buffer, sizeof (buffer), 0);
        if (got <= 0 || ! sendAll (to, buffer, (size_t) got))
            break;
        total += (uint64_t) got;
    }
    ::shutdown (to, SHUT_WR);
    ::shutdown (from, SHUT_RD);
    return total;
}

void closeRoom (const std::string& code)
{
    int hostFd = -1;
    {
        std::lock_guard<std::mutex> sl (g_lock);
        const auto it = g_rooms.find (code);
        if (it == g_rooms.end())
            return;
        hostFd = it->second.hostControlFd;
        g_rooms.erase (it);

        for (auto p = g_pending.begin(); p != g_pending.end();)
        {
            if (p->second.roomCode == code)
            {
                ::close (p->second.musicianFd);
                p = g_pending.erase (p);
            }
            else
                ++p;
        }
    }
    if (hostFd >= 0)
        ::close (hostFd);
    logLine ("room " + code + " closed");
}

//==============================================================================
void runHostControl (int fd, const std::string& hostName)
{
    std::string code;
    {
        std::lock_guard<std::mutex> sl (g_lock);
        if ((int) g_rooms.size() >= kMaxRooms)
        {
            sendLine (fd, "ERR full\n");
            ::close (fd);
            return;
        }
        do { code = newRoomCode(); } while (g_rooms.count (code) != 0);
        g_rooms[code] = Room { fd, 0, hostName };
    }

    if (! sendLine (fd, "ROOM " + code + "\n"))
    {
        closeRoom (code);
        return;
    }
    logLine ("room " + code + " opened by " + hostName);

    // Keep-alive loop: anything the host sends is ignored; a timeout just
    // triggers a PING so NAT mappings stay warm. EOF/error ends the room.
    setRecvTimeout (fd, kPingIntervalS);
    char scratch[256];
    for (;;)
    {
        const ssize_t got = ::recv (fd, scratch, sizeof (scratch), 0);
        if (got > 0)
            continue;
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            if (sendLine (fd, "PING\n"))
                continue;
        }
        break;
    }
    closeRoom (code);
}

void runMusicianJoin (int fd, const std::string& code, const std::string& musicianName)
{
    std::string token = newPeerToken();
    int hostFd = -1;
    {
        std::lock_guard<std::mutex> sl (g_lock);
        const auto it = g_rooms.find (code);
        if (it == g_rooms.end())
        {
            vlog ("join " + code + " by " + musicianName + ": room not found");
            sendLine (fd, "ERR no-such-room\n");
            ::close (fd);
            return;
        }
        if (it->second.numPending >= kMaxPendingPerRoom)
        {
            vlog ("join " + code + " by " + musicianName + ": room busy");
            sendLine (fd, "ERR busy\n");
            ::close (fd);
            return;
        }
        ++it->second.numPending;
        hostFd = it->second.hostControlFd;
        g_pending[token] = Pending { fd, code, musicianName, ::time (nullptr) };
    }
    vlog ("join " + code + " by " + musicianName + ": accepted, asking host to pair");

    // Confirm to the musician, then ask the host to open a data connection.
    // The musician may already start its BandJam handshake; those bytes just
    // wait in the socket buffer until the pipe is up.
    bool ok = sendLine (fd, "OK\n");
    {
        std::lock_guard<std::mutex> sl (g_lock);
        if (ok && g_pending.count (token) != 0)
            ok = sendLine (hostFd, "PEER " + token + "\n");
    }

    if (! ok)
    {
        std::lock_guard<std::mutex> sl (g_lock);
        const auto it = g_pending.find (token);
        if (it != g_pending.end())
        {
            ::close (it->second.musicianFd);
            if (const auto room = g_rooms.find (code); room != g_rooms.end())
                --room->second.numPending;
            g_pending.erase (it);
        }
    }
    // Socket ownership moved to g_pending; the janitor cleans up stale entries.
}

void runHostData (int fd, const std::string& token)
{
    int musicianFd = -1;
    std::string code, musicianName;
    {
        std::lock_guard<std::mutex> sl (g_lock);
        const auto it = g_pending.find (token);
        if (it != g_pending.end())
        {
            musicianFd   = it->second.musicianFd;
            code         = it->second.roomCode;
            musicianName = it->second.musicianName;
            if (const auto room = g_rooms.find (code); room != g_rooms.end())
                --room->second.numPending;
            g_pending.erase (it);
        }
    }
    if (musicianFd < 0)
    {
        vlog ("data connection: unknown token");
        sendLine (fd, "ERR no-such-token\n");
        ::close (fd);
        return;
    }

    vlog ("room " + code + ": pipe up (" + musicianName + ")");
    const time_t started = ::time (nullptr);

    // Low-latency piping for the live audio path.
    int yes = 1;
    ::setsockopt (fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof (yes));
    ::setsockopt (musicianFd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof (yes));
    setRecvTimeout (fd, 0);
    setRecvTimeout (musicianFd, 0);

    uint64_t toHost = 0;
    std::thread up ([fd, musicianFd, &toHost] { toHost = pipeOneWay (musicianFd, fd); });
    const uint64_t toMusician = pipeOneWay (fd, musicianFd);
    up.join();

    ::close (fd);
    ::close (musicianFd);

    const long seconds = (long) (::time (nullptr) - started);
    vlog ("room " + code + ": pipe closed after " + std::to_string (seconds) + "s"
          + " (musician->host " + megabytes (toHost)
          + ", host->musician " + megabytes (toMusician)
          + ", musician " + musicianName + ")");
}

void runConnection (int fd)
{
    setRecvTimeout (fd, kFirstLineTimeoutS);

    std::string line;
    if (! readLine (fd, line))
    {
        vlog ("connection dropped before greeting (scanner?)");
        ::close (fd);
        return;
    }

    if (line.rfind ("HOST", 0) == 0)
    {
        // "HOST <ver> [name...]" - the version is informational only.
        std::string name = "unknown";
        const auto firstSpace = line.find (' ');
        if (firstSpace != std::string::npos)
        {
            const auto nameStart = line.find (' ', firstSpace + 1);
            if (nameStart != std::string::npos)
                name = sanitizeName (line.substr (nameStart + 1));
        }
        runHostControl (fd, name);
    }
    else if (line.rfind ("JOIN ", 0) == 0 && line.size() > 5)
    {
        // "JOIN <code> [name...]"
        std::string rest = line.substr (5);
        std::string name = "unknown";
        const auto space = rest.find (' ');
        if (space != std::string::npos)
        {
            name = sanitizeName (rest.substr (space + 1));
            rest = rest.substr (0, space);
        }
        for (auto& c : rest) c = (char) ::toupper ((unsigned char) c);
        runMusicianJoin (fd, rest, name);
    }
    else if (line.rfind ("DATA ", 0) == 0 && line.size() > 5)
    {
        runHostData (fd, line.substr (5));
    }
    else
    {
        vlog ("bad request: \"" + line.substr (0, 40) + "\"");
        sendLine (fd, "ERR bad-request\n");
        ::close (fd);
    }
}

/** Closes musicians whose host never opened the data connection. */
void runJanitor()
{
    for (;;)
    {
        ::sleep (5);
        std::lock_guard<std::mutex> sl (g_lock);
        const time_t now = ::time (nullptr);
        for (auto it = g_pending.begin(); it != g_pending.end();)
        {
            if (now - it->second.created > kPendingTimeoutS)
            {
                vlog ("room " + it->second.roomCode + ": waiting musician " + it->second.musicianName
                      + " timed out (host never paired)");
                ::close (it->second.musicianFd);
                if (const auto room = g_rooms.find (it->second.roomCode); room != g_rooms.end())
                    --room->second.numPending;
                it = g_pending.erase (it);
            }
            else
                ++it;
        }
    }
}
} // namespace

//==============================================================================
int main (int argc, char** argv)
{
    ::signal (SIGPIPE, SIG_IGN);
    ::signal (SIGUSR1, toggleVerbose);

    int port = kDefaultPort;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp (argv[i], "-v") == 0)
            g_verbose.store (true);
        else if (std::atoi (argv[i]) > 0)
            port = std::atoi (argv[i]);
    }

    const int listener = ::socket (AF_INET, SOCK_STREAM, 0);
    if (listener < 0)
    {
        std::perror ("socket");
        return 1;
    }

    int yes = 1;
    ::setsockopt (listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof (yes));

    sockaddr_in addr {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons ((uint16_t) port);

    if (::bind (listener, (sockaddr*) &addr, sizeof (addr)) < 0)
    {
        std::perror ("bind");
        return 1;
    }
    if (::listen (listener, 32) < 0)
    {
        std::perror ("listen");
        return 1;
    }

    logLine ("bandjam-relay listening on port " + std::to_string (port));
    logLine (std::string ("activity log: ") + (g_verbose.load() ? "ON" : "OFF")
             + "  (toggle with: systemctl kill -s USR1 bandjam-relay)");
    std::thread (runJanitor).detach();

    for (;;)
    {
        const int client = ::accept (listener, nullptr, nullptr);
        if (client < 0)
            continue;
        std::thread (runConnection, client).detach();
    }
}
