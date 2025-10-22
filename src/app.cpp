// src/app.cpp
#include "log.hpp"

#include <iostream>
#include <fstream> 
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>

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
    std::ofstream log(opt_.log_path, std::ios::app);
    if (!log.is_open()) {
        std::cerr << "Error: cannot open log file " << opt_.log_path << "\n";
        return;
    }
}

void App::run_daemon() {
    
}
