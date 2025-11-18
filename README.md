# m – Multithreaded IPC Demo

A small C++ project demonstrating:
- Safe multithreading with `std::thread` and mutex-protected file access.  
- SIGINT → graceful stop; SIGHUP → log rotation; use sigaction with handlers that only set atomic flags, real work done in main loop. 
- Inter-process communication with UNIX domain sockets for control commands.  
- POSIX shared memory (`shm_open`, `mmap`) to publish runtime stats.  

Shows how to combine threads, signals, IPC, and shared resources in a simple, clean design.
