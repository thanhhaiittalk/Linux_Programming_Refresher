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

    umask(027);
    if (chdir("/") != 0) {
        std::perror("chdir");
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
    fd_ = ::open(opt_.log_path.c_str(),
                  O_CREAT | O_WRONLY | O_APPEND,
                  0644);
    if (fd_ < 0) {
        std::cerr << "failed to open log file: "
                  << opt_.log_path
                  << " err=" << std::strerror(errno) << "\n";
        return;
    }

    std::thread t1(&App::thread_heartbeat, this);
    std::thread t2(&App::thread_loadavg,   this);
    std::thread t3(&App::thread_sync,      this);

    t1.join();
    t2.join();
    t3.join();
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
            ::fsync(fd_);
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