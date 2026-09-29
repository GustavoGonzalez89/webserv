#ifndef CONFIGPARSER_HPP
# define CONFIGPARSER_HPP

#include "Server.hpp"

#include <iostream>
#include <string>
#include <cstring>
#include <vector>
#include <map>
#include <stack>
#include <fstream>
#include <cstdlib>
#include <sstream>
#include <algorithm>
#include <iterator>

/*
** ConfigParser — loads and tokenizes an nginx-style .conf file.
** Builds a list of Server objects, one per server block found.
*/
class ConfigParser {
public:
    ConfigParser(std::string absConfigPath);
    ConfigParser(const ConfigParser &other);
    ~ConfigParser();
    ConfigParser &operator=(const ConfigParser &other);

    std::vector<Server> getServerVector();
    std::string         getConfigFile() const;

private:
    std::vector<Server> _srv_list;
    std::string const   _cfg_path;

    ConfigParser();
    std::string parseConfigFile(std::string file);
};


#endif
