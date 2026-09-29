#ifndef SERVER_HPP
# define SERVER_HPP

#include "Location.hpp"

#include <exception>
#include <iostream>
#include <string>
#include <cstring>
#include <vector>
#include <map>
#include <set>
#include <stack>
#include <fstream>
#include <cstdlib>
#include <sstream>
#include <algorithm>
#include <sys/select.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>

/*
** Server — holds the parsed configuration for one server block,
** owns the listening socket, and drives the select() event loop.
*/
class Server {
public:
    std::vector<std::pair<int, std::string> > cSockets;

    Server(std::string name, std::string host, int listen, std::string root,
           int bodySize, std::string index, std::string errors, std::string locations);
    Server(const Server &other);
    ~Server();
    Server &operator=(const Server &other);

    int  start();
    void printInfo();

    const std::vector<Location> getLocation()    const;
    std::string                 getServerName()  const;
    std::string                 getHost()        const;
    int                         getListen()      const;
    std::string                 getRoot()        const;
    int                         getMaxBodySize() const;
    std::string                 getIndex()       const;
    const std::map<int, std::string>& getErrorPageMap() const;
    std::string                 getErrorPage(int code)  const;
    void                        setSocket(int n);
    int                         getSocket()      const;

    static void run(std::vector<Server>& servers);

private:
    std::string const           _sv_name;
    std::string const           _sv_host;
    int         const           _port;
    std::string const           _sv_root;
    int         const           _max_body;
    std::string const           _sv_index;
    std::map<int, std::string>  _err_map;
    std::vector<Location>       _loc_list;
    mutable size_t              _loc_iter;
    mutable size_t              _loc_done;
    struct sockaddr_in          _sock_addr;
    int                         _listen_fd;

    Server();
    void loadErrorPages(const std::string& error_patterns);
    void loadLocationBlocks(const std::string& location_patterns);
};

std::ostream &operator<<(std::ostream &stream, const Server &server);

#endif
