#include "log.hpp"
#include <getopt.h>
#include <iostream>
#include <cstdlib>
#include <algorithm>

Cli::Cli(int argc, char** argv)
    : argc_(argc), argv_(argv) {}

void Cli::print_help(const char* prog) {
    std::cout <<
"Usage: " << (prog ? prog : "log") << " [options]\n"
"  --foreground           Run in foreground (default)\n"
"  --daemon               Run in background (daemonize)\n"
"  --log <path>           Log file path (default: /tmp/videolog.txt)\n"
"  --sync-every <sec>     Heartbeat period in seconds (default: 5)\n"
"  --help                 Show this help\n";
}

int Cli::parse() {
    static const option kLong[] = {
        {"foreground",  no_argument,       nullptr, 'f'},
        {"daemon",      no_argument,       nullptr, 'd'},
        {"log",         required_argument, nullptr, 'l'},
        {"sync-every",  required_argument, nullptr, 's'},
        {"help",        no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    // Reset getopt's global state (useful if tests call parse multiple times)
    optind = 1;

    int c;
    while ((c = getopt_long(argc_, argv_, "fdl:s:h", kLong, nullptr)) != -1) {
        switch (c) {
            case 'f':
                opt_.foreground = true;              // last flag wins
                break;
            case 'd':
                opt_.foreground = false;             // last flag wins
                break;
            case 'l':
                opt_.log_path = optarg ? optarg : opt_.log_path;
                break;
            case 's':
                opt_.sync_every = std::max(1, std::atoi(optarg));
                break;
            case 'h':
                print_help(argv_ ? argv_[0] : "log");
                std::exit(0);
            default:
                return 1; // parse error
        }
    }
    return 0;
}

// Back-compat wrapper
Options parse_cli(int argc, char** argv) {
    Cli cli(argc, argv);
    if (cli.parse() != 0) std::exit(1);
    return cli.options();
}
