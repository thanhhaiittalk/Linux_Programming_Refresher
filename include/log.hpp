#pragma once
#include <string>

// Runtime options
struct Options {
    bool foreground = true;
    std::string log_path = "/tmp/videolog.txt";
    int sync_every = 5;
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
    explicit App(const Options& opt);
    int run();

private:
    Options opt_;
    void run_foreground();
    void run_daemon();
};
