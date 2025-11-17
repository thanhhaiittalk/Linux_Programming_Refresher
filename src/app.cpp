// src/app.cpp
#include "log.hpp"
#include "shared_stats.hpp"

#include <sys/mman.h>
#include <sys/stat.h>
#include <iostream>
#include <fstream> 
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>     
#include <sys/stat.h>
#include <fstream>
#include <sstream>
#include <thread>
#include <cstring>
#include <atomic>
#include <csignal>
#include <sys/socket.h>
#include <sys/un.h>
#include <vector>
#include <poll.h> 
// ------------ static atomics ------------
std::atomic<bool> App::stop_requested_{false};
std::atomic<bool> App::rotate_requested_{false};
std::atomic<bool> App::sigint_logged_{false};

// Return singleton instance (first call with Options creates it)
App& App::instance(const Options& opt) {
    static App inst(opt);
    return inst;
}

// Overload for later calls without Options
App& App::instance() {
    return instance(Options{});
}

App::App(const Options& opt) : opt_(opt) {}

int App::run() {
    if (opt_.foreground) {
        run_foreground();
    } else {
        run_daemon();
    }
    return 0;
}

void App::run_foreground() {
    std::cout << "[foreground] pid=" << getpid() << "\n";
    std::cout << "Log path: " << opt_.log_path << "\n";
    std::cout << "Sync every: " << opt_.sync_every << "s\n";

 // open log file for append
   log();
}

void App::run_daemon() {
    // Print while still attached to the terminal
    std::cout << "[parent] starting daemonization, pid=" << getpid() << "\n";

    // First fork
    pid_t pid = fork();
    if (pid < 0) {
        std::perror("fork");
        std::exit(1);
    }
    if (pid > 0) {
        std::cout << "[parent] forked child pid=" << pid << "\n";
        std::exit(0);
    }

    // Create new session
    if (setsid() < 0) {
        std::perror("setsid");
        std::exit(1);
    }

    // Second fork to fully detach
    pid = fork();
    if (pid < 0) {
        std::perror("fork");
        std::exit(1);
    }
    if (pid > 0) {
        std::exit(0);
    }

    std::cout << "[daemon] running, pid=" << getpid() << "\n";
    std::cout << "Log path: " << opt_.log_path
              << ", sync every: " << opt_.sync_every << "s\n";

    // Redirect stdio to /dev/null
    (void)freopen("/dev/null", "r", stdin);
    (void)freopen("/dev/null", "w", stdout);
    (void)freopen("/dev/null", "w", stderr);

    log();
}

void App::log() {
    // open file
    open_logfile();
    if (fd_ < 0) {
        std::cerr << "failed to open log file: "
                  << opt_.log_path
                  << " err=" << std::strerror(errno) << "\n";
        return;
    }

    if (!shared_init_writer()) {
        std::cerr << "warning: shared memory init failed, continuing without shm\n";
    }
    // install SIGINT / SIGHUP handlers
    install_signal_handlers();

    // Start control interface (create socket, spawn tCTRL_)
    if (!start_control_interface("/tmp/logd.sock")) {
        std::cerr << "warning: control interface failed to start\n";
        // continue anyway — the daemon can run without control socket
    }

    std::thread tHB_(&App::thread_heartbeat, this);
    std::thread tSYS_(&App::thread_loadavg,   this);
    std::thread tSYNC_(&App::thread_sync,      this);

    // main thread just waits until stop_requested_ becomes true
    while (!stop_requested_.load(std::memory_order_relaxed)) {
        // lightweight sleep
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    graceful_shutdown();
}

void App::safe_write_all(int fd, const std::string &line) {
    const char* buf = line.c_str();
    size_t total = line.size();
    size_t sent = 0;

    while (sent < total) {
        ssize_t n = ::write(fd, buf + sent, total - sent);
        if (n < 0) {
            break;
        }
        sent += static_cast<size_t>(n);
    }
}
std::string App::timestamp_now() {
    using namespace std::chrono;

    auto now = system_clock::now();
    auto secs = time_point_cast<seconds>(now);
    auto micros = duration_cast<microseconds>(now - secs).count();

    std::time_t t = system_clock::to_time_t(now);
    std::tm tm_local;
    localtime_r(&t, &tm_local);

    char buf[64];
    // date + time first
    std::snprintf(buf, sizeof(buf),
                  "%04d-%02d-%02d %02d:%02d:%02d.%06ld",
                  tm_local.tm_year + 1900,
                  tm_local.tm_mon + 1,
                  tm_local.tm_mday,
                  tm_local.tm_hour,
                  tm_local.tm_min,
                  tm_local.tm_sec,
                  (long)micros);
    return std::string(buf);
}

void App::thread_heartbeat() {
    int tick = 0;
    while (true) {
        std::ostringstream oss;
        oss << timestamp_now()
            << " [HB] alive tick=" << tick++
            << "\n";

        {
            std::lock_guard<std::mutex> lock(logMutex_);
            safe_write_all(fd_, oss.str());
        }

        if (shm_ptr_) {
            // lock the shared mutex (best-effort robust handling omitted)
            if (pthread_mutex_lock(&shm_ptr_->lock) == 0) {
                shm_ptr_->value = static_cast<uint64_t>(tick); // dummy payload
                shm_ptr_->writer = static_cast<pid_t>(::getpid());
                pthread_mutex_unlock(&shm_ptr_->lock);
            } else {
                // optionally log or ignore if lock failed
            }
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void App::thread_loadavg() {
    while (true) {
        double one=0, five=0, fifteen=0;
        bool ok = read_loadavg(one, five, fifteen);

        std::ostringstream oss;
        oss << timestamp_now()
            << " [SYS] loadavg=";

        if (ok) {
            oss << one << "," << five << "," << fifteen;
        } else {
            oss << "ERR";
        }
        oss << "\n";

        {
            std::lock_guard<std::mutex> lock(logMutex_);
            safe_write_all(fd_, oss.str());
        }

        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

void App::thread_sync() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(opt_.sync_every));
        {
            // protect fsync too, so we don't sync while another thread is mid-write
            std::lock_guard<std::mutex> lock(logMutex_);
            if (rotate_requested_.load(std::memory_order_relaxed)) {
                rotate_requested_.store(false, std::memory_order_relaxed);
                rotate_logfile_locked(); // does its own fsync/close/reopen and writes "[CTRL] log rotated"
            } else {
                if (fd_ >= 0) {
                     std::ostringstream oss;
                     oss << timestamp_now()
                         << " SYNCED" 
                         << "\n";
                     {
                         safe_write_all(fd_, oss.str());
                     }
                     ::fsync(fd_);
                     // you could also write a debug sync line if you like
                     // e.g. "[SYNC] flushed"
                }
            }
        }
        // (optional) you could also log a debug line here if you want:
        // "[SYNC] flushed"
    }
}

bool App::read_loadavg(double &one, double &five, double &fifteen) {
    std::ifstream f("/proc/loadavg");
    if (!f.is_open()) return false;
    f >> one >> five >> fifteen;
    return true;
}

// ------------ open logfile helper ------------
void App::open_logfile() {
    fd_ = ::open(opt_.log_path.c_str(),
                 O_CREAT | O_WRONLY | O_APPEND,
                 0644);
}

// ------------ signal handling ------------
void App::install_signal_handlers() {
    struct sigaction sa {};
    sa.sa_handler = &App::signal_trampoline;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    // SIGINT -> graceful stop
    if (sigaction(SIGINT, &sa, nullptr) < 0) {
        std::perror("sigaction(SIGINT)");
    }

    // SIGHUP -> log rotation
    if (sigaction(SIGHUP, &sa, nullptr) < 0) {
        std::perror("sigaction(SIGHUP)");
    }
}

void App::signal_trampoline(int sig){
    App &app = App::instance(); // singleton
    if (sig == SIGINT) {
        app.handle_sigint(); // set flags
    } else if (sig == SIGHUP) {
        app.handle_sighup(); // set rotate flag
    }
} // static -> calls into singleton

void App::handle_sigint() {
    stop_requested_.store(true, std::memory_order_relaxed);

    // we don't do heavy I/O here, but we want to remember that we owe a "[CTRL]" line
    sigint_logged_.store(true, std::memory_order_relaxed);
}  // set stop flag + log ctrl line
void App::handle_sighup(){
    rotate_requested_.store(true, std::memory_order_relaxed);
}  // set rotate flag
void App::rotate_logfile_locked(){
        // close current fd
    if (fd_ >= 0) {
        ::fsync(fd_);
        ::close(fd_);
        fd_ = -1;
    }

    // build rotated filename: "<orig>.<timestamp>"
    std::string rotated_name;
    {
        std::ostringstream rn;
        rn << opt_.log_path << "." << timestamp_now();
        rotated_name = rn.str();
    }

    // rename() old path -> rotated_name
    // note: if rename fails, we just continue and try to reopen anyway
    ::rename(opt_.log_path.c_str(), rotated_name.c_str());

        // reopen fresh log file at original path
    open_logfile();

    // after reopen, write a control line
    if (fd_ >= 0) {
        std::ostringstream oss;
        oss << timestamp_now()
            << " [CTRL] log rotated\n";
        safe_write_all(fd_, oss.str());
    }

} // does real rotation under mutex
void App::graceful_shutdown(){
    // stop control interface first so control thread unblocks and exits
    stop_control_interface();

    // Unmap shared memory
    shared_unmap_writer();

    // tell worker threads to exit (they read stop_requested_)
    // join them
    if (tHB_.joinable())   tHB_.join();
    if (tSYS_.joinable())  tSYS_.join();
    if (tSYNC_.joinable()) tSYNC_.join();

    // final sync + "[CTRL] shutting down"
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        if (fd_ >= 0) {
            if (sigint_logged_.load(std::memory_order_relaxed)) {
                std::ostringstream oss;
                oss << timestamp_now()
                    << " [CTRL] received SIGINT -> shutting down\n";
                safe_write_all(fd_, oss.str());
            }

            ::fsync(fd_);
            ::close(fd_);
            fd_ = -1;
        }
    }

    // exit(0) here is optional; caller can also just return
    std::_Exit(0);
}     // join threads, fsync, close fd

/*UNIX Domain socket*/
// correct — no default in definition
bool App::start_control_interface(const std::string& sock_path) {
    // If control thread already running, don't start again.
    if (tCTRL_.joinable() || ctrl_srv_fd_ >= 0) {
        return false;
    }

    // save the socket path
    ctrl_sock_path_ = sock_path;

    // remove stale socket file if present
    ::unlink(ctrl_sock_path_.c_str());

    // create unix domain stream socket
    ctrl_srv_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctrl_srv_fd_ < 0) {
        perror("control socket");
        ctrl_srv_fd_ = -1;
        return false;
    }

    // set CLOEXEC so child/exec'd processes won't inherit the FD
    int fdflags = fcntl(ctrl_srv_fd_, F_GETFD, 0);
    if (fdflags != -1) {
        fcntl(ctrl_srv_fd_, F_SETFD, fdflags | FD_CLOEXEC);
    }

        // prepare address
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (ctrl_sock_path_.size() >= sizeof(addr.sun_path)) {
        std::cerr << "control socket path too long: " << ctrl_sock_path_ << "\n";
        ::close(ctrl_srv_fd_);
        ctrl_srv_fd_ = -1;
        return false;
    }
    std::strncpy(addr.sun_path, ctrl_sock_path_.c_str(), sizeof(addr.sun_path) - 1);

    // bind
    if (::bind(ctrl_srv_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind control socket");
        ::close(ctrl_srv_fd_);
        ctrl_srv_fd_ = -1;
        // ensure no stale file left
        ::unlink(ctrl_sock_path_.c_str());
        return false;
    }

    // set socket file permissions: owner read/write (adjust if you want group access)
    ::chmod(ctrl_sock_path_.c_str(), S_IRUSR | S_IWUSR);

    // listen
    if (::listen(ctrl_srv_fd_, 5) < 0) {
        perror("listen control socket");
        ::close(ctrl_srv_fd_);
        ctrl_srv_fd_ = -1;
        ::unlink(ctrl_sock_path_.c_str());
        return false;
    }

        // clear stop flag and ignore SIGPIPE to avoid termination on broken pipes
    ctrl_stop_.store(false, std::memory_order_relaxed);
    ::signal(SIGPIPE, SIG_IGN);

     // spawn control thread that runs control_thread_fn()
    try {
        tCTRL_ = std::thread(&App::control_thread_fn, this);
    } catch (...) {
        // thread creation failed: cleanup
        ::close(ctrl_srv_fd_);
        ctrl_srv_fd_ = -1;
        ::unlink(ctrl_sock_path_.c_str());
        return false;
    }

    return true;
}

void App::stop_control_interface() {
        // Step 1: signal control thread to stop
    ctrl_stop_.store(true, std::memory_order_relaxed);

    // Step 2: close listening socket if open
    if (ctrl_srv_fd_ >= 0) {
        // shutdown wakes up accept() or poll() inside control_thread_fn
        ::shutdown(ctrl_srv_fd_, SHUT_RDWR);
        ::close(ctrl_srv_fd_);
        ctrl_srv_fd_ = -1;
    }

    // Step 3: wait for control thread to finish
    if (tCTRL_.joinable()) {
        tCTRL_.join();
    }

    // Step 4: remove the socket file
    if (!ctrl_sock_path_.empty()) {
        ::unlink(ctrl_sock_path_.c_str());
    }
}

void App::control_thread_fn() {
    if (ctrl_srv_fd_ < 0) return;

    constexpr int POLL_TIMEOUT_MS = 500;
    constexpr size_t BUF_SZ = 4096;

    // Ensure SIGPIPE won't kill the thread (safe-guard)
    ::signal(SIGPIPE, SIG_IGN);

    while (!ctrl_stop_.load(std::memory_order_relaxed)) {
        struct pollfd pfd{};
        pfd.fd = ctrl_srv_fd_;
        pfd.events = POLLIN;

        int ret = ::poll(&pfd, 1, POLL_TIMEOUT_MS);
        if (ret < 0) {
            if (errno == EINTR) continue;
            // serious poll error: abort loop
            perror("poll(control)");
            break;
        } else if (ret == 0) {
            // timeout - check stop flag again
            continue;
        }


        if (pfd.revents & (POLLERR | POLLNVAL)) {
            // fatal on listen fd
            perror("poll error or invalid");
            break;
        }

        if (pfd.revents & POLLIN) {
            while (true){
                int cfd = ::accept(ctrl_srv_fd_, nullptr, nullptr);
                if (cfd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        // no more pending connections
                        break;
                    }
                    if (errno == EINTR) {
                        // try again
                        continue;
                    }
                    // real error on accept — log and break accept loop
                    perror("accept(control)");
                    break;
                }

                // Set close-on-exec on client fd
                int fdflags = fcntl(cfd, F_GETFD, 0);
                if (fdflags != -1) {
                    fcntl(cfd, F_SETFD, fdflags | FD_CLOEXEC);
                }

                // Read up to BUF_SZ from client. We take the first line as the command.
                std::string cmdline;
                {
                    char buf[BUF_SZ];
                    ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
                    if (n > 0) {
                        cmdline.assign(buf, buf + n);
                        // extract only the first line if multiple lines in buffer
                        auto pos = cmdline.find('\n');
                        if (pos != std::string::npos) {
                            cmdline.erase(pos); // drop trailing data after first newline
                        }
                        // strip trailing CR if present
                        if (!cmdline.empty() && cmdline.back() == '\r') cmdline.pop_back();
                    } else if (n == 0) {
                        // client closed immediately, nothing to do
                        ::close(cfd);
                        continue;
                    } else {
                        // recv error
                        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                            // treat as no data
                            ::close(cfd);
                            continue;
                        } else {
                            // other error
                            perror("recv(control)");
                            ::close(cfd);
                            continue;
                        }
                    }
                }

                // handle the command (inside this thread)
                handle_control_command(cmdline, cfd);

                // Close client connection (handler already wrote reply)
                ::shutdown(cfd, SHUT_RDWR);
                ::close(cfd);


                // If ctrl_stop_ became true while handling command, break out early
                if (ctrl_stop_.load(std::memory_order_relaxed)) break;
            } 
        }
    }// end main loop
    
    if (ctrl_srv_fd_ >= 0) {
        ::close(ctrl_srv_fd_);
        ctrl_srv_fd_ = -1;
    }

    if (!ctrl_sock_path_.empty()) {
        ::unlink(ctrl_sock_path_.c_str());
    }
}

void App::handle_control_command(const std::string& cmd, int client_fd) {
    // Trim whitespace (leading/trailing)
    auto trim = [](const std::string &s) -> std::string {
        size_t a = 0;
        while (a < s.size() && isspace((unsigned char)s[a])) ++a;
        size_t b = s.size();
        while (b > a && isspace((unsigned char)s[b-1])) --b;
        return s.substr(a, b - a);
    };

    std::string line = trim(cmd);
    std::string reply;

        // Tokenize input
    std::istringstream iss(line);
    std::vector<std::string> parts;
    std::string tok;
    while (iss >> tok) parts.push_back(tok);

    if (line.empty()) {
       reply = "ERR empty command\n";
    } else if (parts[0] == "status") {
        // handle status
        int sync_val;
        {
            std::lock_guard<std::mutex> lk(logMutex_);
            sync_val = opt_.sync_every;
        }
        std::ostringstream oss;
        oss << "OK status: running sync_every=" << sync_val << "\n";
        reply = oss.str();
    }
    else if (parts[0] == "rotate") {
        // rotate logs
        {
            std::lock_guard<std::mutex> lk(logMutex_);
            rotate_logfile_locked();
        }
        reply = "OK rotate\n";
    }
    else if (parts[0] == "shutdown") {
        stop_requested_.store(true, std::memory_order_relaxed);
        sigint_logged_.store(true, std::memory_order_relaxed);
        reply = "OK shutdown\n";
    }
    else if (parts[0] == "sync_every"){
        if (parts.size() != 2) {
            reply = "ERR usage: sync_every <seconds>\n";
        } else {
            try {
                int n = std::stoi(parts[1]);
                if (n <= 0) {
                    reply = "ERR invalid number\n";
                } else {
                    {
                        std::lock_guard<std::mutex> lk(logMutex_);
                        opt_.sync_every = n;
                    }
                    reply = "OK sync_every " + std::to_string(n) + "\n";
                }
            } catch (...) {
                reply = "ERR invalid number\n";
            }
        }
    } 
    else {
        reply = "ERR unknown\n";
    }

    // send reply (best-effort)
    const char* buf = reply.data();
    size_t to_send = reply.size();
    size_t sent = 0;

    while (sent < to_send) {
        ssize_t n = ::send(client_fd, buf + sent, to_send - sent, 0);
        if (n > 0) { sent += static_cast<size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        break; // EPIPE or error
    }
}

/*Shared memory*/
bool App::shared_init_writer() {
    // If already initialized, nothing to do
    if (shm_ptr_ != nullptr || shm_fd_ >= 0) return true;

    // Open/create shared memory
    shm_fd_ = ::shm_open(SHM_STATS_NAME, O_CREAT | O_RDWR, 0644);
    if (shm_fd_ < 0) {
        std::cerr << "shm_open failed: " << std::strerror(errno) << "\n";
        shm_fd_ = -1;
        return false;
    }

    // Ensure size
    if (::ftruncate(shm_fd_, static_cast<off_t>(sizeof(SharedStats))) < 0) {
        std::cerr << "ftruncate(shm) failed: " << std::strerror(errno) << "\n";
        ::close(shm_fd_);
        shm_fd_ = -1;
        ::shm_unlink(SHM_STATS_NAME); // try to clean up
        return false;
    }

    // mmap it
    void* p = ::mmap(nullptr, sizeof(SharedStats),
                     PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
    if (p == MAP_FAILED) {
        std::cerr << "mmap(shm) failed: " << std::strerror(errno) << "\n";
        ::close(shm_fd_);
        shm_fd_ = -1;
        ::shm_unlink(SHM_STATS_NAME);
        return false;
    }

    shm_ptr_ = reinterpret_cast<SharedStats*>(p);

    // Zero memory first (important before initializing a pthread mutex in-shm)
    std::memset(shm_ptr_, 0, sizeof(SharedStats));

    // Initialize mutex attr for process-shared mutex
    pthread_mutexattr_t mattr;
    if (pthread_mutexattr_init(&mattr) != 0) {
        std::cerr << "pthread_mutexattr_init failed\n";
        // cleanup
        shared_unmap_writer();
        return false;
    }

    if (pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED) != 0) {
        std::cerr << "pthread_mutexattr_setpshared failed\n";
        pthread_mutexattr_destroy(&mattr);
        shared_unmap_writer();
        return false;
    }

    // Optionally make robust to handle owner death
    // pthread_mutexattr_setrobust(&mattr, PTHREAD_MUTEX_ROBUST);

    if (pthread_mutex_init(&shm_ptr_->lock, &mattr) != 0) {
        std::cerr << "pthread_mutex_init failed\n";
        pthread_mutexattr_destroy(&mattr);
        shared_unmap_writer();
        return false;
    }

    pthread_mutexattr_destroy(&mattr);

    // Initialize data fields
    shm_ptr_->value = 0;
    shm_ptr_->writer = 0;

    // keep the FD open (allows other processes to open it too)
    return true;
}

void App::shared_unmap_writer() {
    if (shm_ptr_) {
        // Best-effort: destroy mutex (only if no other processes using it)
        // Note: destroying a process-shared mutex while other processes exist is UB.
        // We attempt to destroy but ignore errors.
        pthread_mutex_destroy(&shm_ptr_->lock);

        ::munmap(reinterpret_cast<void*>(shm_ptr_), sizeof(SharedStats));
        shm_ptr_ = nullptr;
    }
    if (shm_fd_ >= 0) {
        ::close(shm_fd_);
        shm_fd_ = -1;
    }
    // Unlink the name so it doesn't linger in /dev/shm after clean shutdown.
    // If you prefer not to unlink (for debugging), remove this call.
    ::shm_unlink(SHM_STATS_NAME);
}