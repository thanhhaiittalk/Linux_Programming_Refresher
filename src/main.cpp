#include "log.hpp"
#include <iostream>

int main(int argc, char** argv) {
    // Parse CLI arguments
    Cli cli(argc, argv);
    if (cli.parse() != 0) {
        std::cerr << "Failed to parse command line.\n";
        return 1;
    }

    // Get parsed options
    const Options& opt = cli.options();

    // Create the main application object
    App app(opt);

    // Run the app (decides foreground or daemon)
    return app.run();
}
