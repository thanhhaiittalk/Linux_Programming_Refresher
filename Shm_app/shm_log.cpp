#include "shared_stats.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <chrono>
#include <thread>
#include <fstream>
#include <atomic>
#include <csignal>

static volatile sig_atomic_t g_stop = 0;
extern "C" void handle_sigint(int) { g_stop = 1; }

int main(int argc, char** argv) {
    std::signal(SIGINT, handle_sigint);
    std::signal(SIGTERM, handle_sigint);

    const char* logfile = "./shm_writer.log";
    std::ofstream logf(logfile, std::ios::app);
    if (!logf.is_open()) {
        std::cerr << "failed to open " << logfile << "\n";
        return 1;
    }

    // Open existing shm created by daemon (do NOT create)
    int fd = ::shm_open(SHM_STATS_NAME, O_RDWR, 0);
    if (fd < 0) {
        logf << "shm_open failed: " << std::strerror(errno) << "\n";
        std::cerr << "shm_open failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    // Map it
    void* p = ::mmap(nullptr, sizeof(SharedStats), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        logf << "mmap failed: " << std::strerror(errno) << "\n";
        std::cerr << "mmap failed: " << std::strerror(errno) << "\n";
        ::close(fd);
        return 1;
    }
    SharedStats* s = reinterpret_cast<SharedStats*>(p);


    pid_t mypid = static_cast<pid_t>(::getpid());
    logf << "shm_writer pid=" << mypid << " started\n";
    std::cout << "shm_writer pid=" << mypid << " started\n";

    uint64_t local_counter = 1;

    while (!g_stop) {
        int r = pthread_mutex_lock(&s->lock);
    if (r == 0) {
        // READ existing values
        uint64_t old_value = s->value;
        pid_t    old_writer = s->writer;

        // Log the read
        auto now = std::chrono::system_clock::now();
        std::time_t tt = std::chrono::system_clock::to_time_t(now);
        logf << "[" << std::ctime(&tt) << "] "
             << "read: value=" << old_value << " writer=" << old_writer << "\n";
        logf.flush();

        // WRITE new values (under the mutex)
        s->value = local_counter++;
        s->writer = mypid;

        // optional: log the write
        tt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        logf << "[" << std::ctime(&tt) << "] "
             << "wrote: value=" << s->value << " writer=" << s->writer << "\n";
        logf.flush();

        pthread_mutex_unlock(&s->lock);
    } else if (r == EOWNERDEAD) {
        // If you compiled with ROBUST mutex attr, handle recovery:
        // We "own" the mutex now and must repair state before making it consistent.
        // Minimal approach: try to make a safe state and mark consistent.
        // NOTE: add real repair logic as needed for your app.
        logf << "got EOWNERDEAD; repairing state\n";
        // repair_shared_state(s);  // if needed
        pthread_mutex_consistent(&s->lock);
        pthread_mutex_unlock(&s->lock);
    } else {
        // other error (EINVAL, etc.) — log and break out so we clean up
        logf << "pthread_mutex_lock failed: " << std::strerror(r) << "\n";
        break;
    }

    // Sleep in small increments and check g_stop so Ctrl-C is responsive.
    for (int i = 0; i < 20 && !g_stop; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    }

    // Cleanup
    ::munmap(p, sizeof(SharedStats));
    ::close(fd);
    logf << "shm_writer pid=" << mypid << " exiting\n";
    logf.close();
    return 0;
}