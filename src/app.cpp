// src/app.cpp
#include "log.hpp"

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
    // install SIGINT / SIGHUP handlers
    install_signal_handlers();

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