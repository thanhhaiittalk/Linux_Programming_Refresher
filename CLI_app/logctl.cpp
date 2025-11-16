#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <cstring>
#include <cerrno>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// default socket path
static constexpr const char* DEFAULT_SOCK = "/tmp/logd.sock";

static void safe_write_all(int fd, const char* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) { sent += static_cast<size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        // otherwise error (EPIPE etc.) — stop trying
        break;
    }
}

int main(int argc, char** argv) {
    std::string sock = DEFAULT_SOCK;
    std::string cmd;

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <command> [args...] [-s /path/to/socket]\n"
                  << "Examples:\n"
                  << "  " << argv[0] << " status\n"
                  << "  " << argv[0] << " rotate\n"
                  << "  " << argv[0] << " shutdown\n"
                  << "  " << argv[0] << " sync_every 5\n";
        return 2;
    }

    // parse args: allow optional -s /socket before or after command
    // build command string from non-option args (first non -s token...rest)
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-s") == 0 || std::strcmp(argv[i], "--socket") == 0) {
            if (i + 1 < argc) { sock = argv[i+1]; i++; continue; }
            std::cerr << "Missing socket path after -s\n";
            return 2;
        }
        args.emplace_back(argv[i]);
    }

    // join args into one line with single spaces and newline terminated
    std::ostringstream oss;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) oss << ' ';
        oss << args[i];
    }
    oss << '\n';
    cmd = oss.str();

    // create socket
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        std::perror("socket");
        return 1;
    }

    // prepare address
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (sock.size() >= sizeof(addr.sun_path)) {
        std::cerr << "socket path too long\n";
        ::close(fd);
        return 1;
    }
    std::strncpy(addr.sun_path, sock.c_str(), sizeof(addr.sun_path) - 1);

    // connect
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "connect(" << sock << ") failed: " << std::strerror(errno) << "\n";
        ::close(fd);
        return 1;
    }

    // send command (use MSG_NOSIGNAL to avoid SIGPIPE)
    safe_write_all(fd, cmd.data(), cmd.size());

    // read reply until EOF
    constexpr size_t BUFSZ = 4096;
    char buf[BUFSZ];
    while (true) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            ssize_t w = ::write(STDOUT_FILENO, buf, static_cast<size_t>(n));
            (void)w;
            continue;
        }
        if (n == 0) {
            // server closed connection cleanly
            break;
        }
        // n < 0
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // no data; continue or break
            continue;
        }
        std::perror("recv");
        break;
    }

    ::close(fd);
    return 0;
}