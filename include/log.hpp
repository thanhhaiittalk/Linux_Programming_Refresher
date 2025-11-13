#pragma once
#include <string>
#include <mutex>
#include <thread>
#include <atomic>

// Runtime options
struct Options {
    bool foreground = true;
    std::string log_path = "./log.txt";
    int sync_every = 1;
};

// --- CLI as a class ---
class Cli {
public:
    Cli(int argc, char** argv);
    int parse();                         // returns 0 on success (may std::exit on --help)
    const Options& options() const { return opt_; }
    static void print_help(const char* prog);

private:
    int argc_;
    char** argv_;
    Options opt_;
};

// (Optional) Back-compat wrapper so existing code still works:
Options parse_cli(int argc, char** argv);

// --- App skeleton (unchanged) ---
class App {
public:
        // Get or create singleton instance
    static App& instance(const Options& opt);
    static App& instance();

    
    int run();

private:
    explicit App(const Options& opt);
    ~App() = default;

    // Disable copy / assignment
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    Options opt_;
    std::mutex logMutex_;
    int fd_ = -1;

    static std::atomic<bool> stop_requested_;
    static std::atomic<bool> rotate_requested_;
    static std::atomic<bool> sigint_logged_;

    // Thread
    std::thread tHB_;
    std::thread tSYS_;
    std::thread tSYNC_;

    // --- Control interface ---
    std::thread tCTRL_;                    // control thread
    int ctrl_srv_fd_ = -1;                 // listening socket fd
    std::string ctrl_sock_path_ = "/tmp/logd.sock";  // socket file path
    std::atomic<bool> ctrl_stop_{false};   // stop flag for control thread
    
    void run_foreground();
    void run_daemon();
    void log();

    void safe_write_all(int fd, const std::string &line) ;
    std::string timestamp_now();
    void thread_heartbeat();
    void thread_loadavg();
    void thread_sync();
    bool read_loadavg(double &one, double &five, double &fifteen);
    void open_logfile();

    // Signal Helper
    void install_signal_handlers();
    static void signal_trampoline(int sig); // static -> calls into singleton
    void handle_sigint();  // set stop flag + log ctrl line
    void handle_sighup();  // set rotate flag

    void rotate_logfile_locked(); // does real rotation under mutex
    void graceful_shutdown();     // join threads, fsync, close fd

    // Start/stop the UNIX control socket server
    bool start_control_interface(const std::string& sock_path = "/tmp/logd.sock");
    void stop_control_interface();

    // Thread function and command handler
    void control_thread_fn();
    void handle_control_command(const std::string& cmd, int client_fd);
};
