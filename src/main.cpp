#include "ConfigParser.hpp"
#include <signal.h>

volatile sig_atomic_t g_running = 1;

static void signal_handler(int) {
    g_running = 0;
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cout << "Wrong number of arguments, expecting only one config file" << std::endl;
        return 1;
    }
    std::string filename(argv[1]);
    if (filename.find_last_of(".") == std::string::npos ||
        filename.substr(filename.find_last_of(".") + 1) != "conf")
    {
        std::cout << "Invalid config file, exiting" << std::endl;
        return 1;
    }

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    ConfigParser parser(argv[1]);

    if (parser.getConfigFile().empty())
    {
        std::cerr << "Error: invalid or empty config file, exiting." << std::endl;
        return 1;
    }

    std::vector<Server> servers = parser.getServerVector();

    for(std::vector<Server>::iterator it = servers.begin(); it != servers.end(); ++it)
    {
        std::cout << *it << std::endl;
    }
    Server::run(servers);

    return 0;
}
