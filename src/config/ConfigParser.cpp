#include "ConfigParser.hpp"

/* Returns true when every opening brace has a matching closing brace. */
static bool checkBraceBalance(const std::string& input) {
    std::stack<char> stk;
    for (std::size_t i = 0; i < input.length(); ++i)
    {
        if (input[i] == '{')
        {
            stk.push('{');
        } else if (input[i] == '}')
        {
            if (stk.empty())
            {
                return false;
            }
            stk.pop();
        }
    }
    return stk.empty();
}

static bool isVisibleChar(char c) {
    return !std::isspace(static_cast<unsigned char>(c));
}

/* Strips lines that begin with '#' (after leading whitespace). */
static void stripCommentedLines(std::string& content) {
    std::istringstream in_stream(content);
    std::ostringstream out_stream;
    std::string line;
    while (std::getline(in_stream, line)) {
        line.erase(line.begin(), std::find_if(line.begin(), line.end(), isVisibleChar));
        if (!line.empty() && line[0] != '#') {
            out_stream << line << '\n';
        }
    }
    content = out_stream.str();
}

/* Removes leading and trailing whitespace from a string. */
static std::string stripWhitespace(const std::string& str) {
    size_t head = str.find_first_not_of(" \t\n\r\f\v");
    size_t tail = str.find_last_not_of(" \t\n\r\f\v");
    if (head == std::string::npos || tail == std::string::npos)
        return "";
    return str.substr(head, tail - head + 1);
}

/*
** Extracts the next "server { ... }" block from `input` starting at `scan_pos`.
** Handles nested location blocks (depth tracking).
** Updates scan_pos to point past the extracted block on return.
*/
static std::string extractServerBlock(const std::string& input, size_t& scan_pos, size_t& sv_found) {
    size_t sv_keyword  = input.find("server {", scan_pos);
    size_t open_br     = input.find("{", scan_pos);
    size_t close_br    = input.find('}', open_br);
    size_t depth       = 0;
    while (sv_keyword != std::string::npos && open_br != std::string::npos && close_br != std::string::npos)
    {
        sv_found = (sv_found == 0) ? 1 : sv_found;
        size_t inner_open = input.find('{', open_br + 1);
        if (inner_open != std::string::npos && inner_open < close_br)
        {
            depth    = (depth == 0) ? 1 : depth;
            open_br  = inner_open;
            close_br = input.find('}', inner_open);
        } else {
            size_t next_open  = input.find('{', close_br);
            size_t next_sv    = input.find("server {", close_br);
            size_t alt_close  = input.find('}', close_br + 1);
            if (depth == 1 && next_open != std::string::npos && next_open < alt_close && next_open < next_sv)
            {
                open_br  = next_open;
                close_br = input.find('}', next_open);
            } else {
                scan_pos = close_br + 1;
                if (depth == 0 || alt_close > next_sv || alt_close == std::string::npos)
                {
                    alt_close = close_br;
                }
                return stripWhitespace(input.substr(sv_keyword + 8, alt_close - sv_keyword - 8));
            }
        }
    }
    if (!sv_found)
    {
        return "";
    }
    return "";
}

/* Extracts the value of the "server_name" directive from a server block. */
static std::string extractServerName(std::string input) {
    const std::string target = "server_name";
    size_t kw_pos = input.find(target);
    std::string sv_name = "";

    while (kw_pos != std::string::npos) {
        bool before_ok = (kw_pos == 0 || std::isspace(input[kw_pos - 1]));
        bool after_ok  = (kw_pos + target.length() < input.length() &&
                          std::isspace(input[kw_pos + target.length()]));

        if (before_ok && after_ok) {
            size_t val_start = kw_pos + target.length();
            size_t sc_pos    = input.find(';', val_start);

            if (sc_pos != std::string::npos) {
                sv_name = stripWhitespace(input.substr(val_start, sc_pos - val_start));
                break;
            }
        }
        kw_pos = input.find(target, kw_pos + 1);
    }

    return sv_name;
}

/* Extracts the "host" directive value (global scope only, not inside location). */
static std::string extractHostAddr(std::string input) {
    size_t loc_pos = input.find("location");
    if (loc_pos != std::string::npos) {
        input = input.substr(0, loc_pos);
    }

    const std::string target = "host";
    size_t kw_pos = input.find(target);

    while (kw_pos != std::string::npos) {
        bool before_ok = (kw_pos == 0 || std::isspace(input[kw_pos - 1]));
        bool after_ok  = std::isspace(input[kw_pos + target.length()]);

        if (before_ok && after_ok) {
            size_t val_start = kw_pos + target.length();
            size_t sc_pos    = input.find(';', val_start);

            if (sc_pos != std::string::npos) {
                return stripWhitespace(input.substr(val_start, sc_pos - val_start));
            }
        }
        kw_pos = input.find(target, kw_pos + 1);
    }

    return "";
}

/* Extracts the port number from the "listen" directive. Returns 0 on failure. */
static int extractPortNum(std::string input) {
    size_t loc_pos = input.find("location");
    if (loc_pos != std::string::npos) {
        input = input.substr(0, loc_pos);
    }

    const std::string target = "listen";
    size_t kw_pos    = input.find(target);
    std::string port_str = "";

    while (kw_pos != std::string::npos) {
        bool before_ok = (kw_pos == 0 || std::isspace(input[kw_pos - 1]));
        bool after_ok  = (kw_pos + target.length() < input.length() &&
                          std::isspace(input[kw_pos + target.length()]));

        if (before_ok && after_ok) {
            size_t val_start = kw_pos + target.length();
            size_t sc_pos    = input.find(';', val_start);

            if (sc_pos != std::string::npos) {
                port_str = stripWhitespace(input.substr(val_start, sc_pos - val_start));
                break;
            }
        }
        kw_pos = input.find(target, kw_pos + 1);
    }

    if (port_str.empty()) return 0;

    for (size_t i = 0; i < port_str.length(); ++i) {
        if (!std::isdigit(port_str[i])) return 0;
    }

    return std::atoi(port_str.c_str());
}

/* Extracts the global "root" directive (ignored inside location blocks). */
static std::string extractRootDir(std::string input) {
    size_t loc_pos = input.find("location");
    if (loc_pos != std::string::npos) {
        input = input.substr(0, loc_pos);
    }

    const std::string target = "root";
    size_t kw_pos      = input.find(target);
    std::string root_path = "";

    while (kw_pos != std::string::npos) {
        bool before_ok = (kw_pos == 0 || std::isspace(input[kw_pos - 1]));
        bool after_ok  = (kw_pos + target.length() < input.length() &&
                          std::isspace(input[kw_pos + target.length()]));

        if (before_ok && after_ok) {
            size_t val_start = kw_pos + target.length();
            size_t sc_pos    = input.find(';', val_start);

            if (sc_pos != std::string::npos) {
                root_path = stripWhitespace(input.substr(val_start, sc_pos - val_start));
                break;
            }
        }
        kw_pos = input.find(target, kw_pos + 1);
    }

    return root_path;
}

/*
** Extracts "client_max_body_size" value in bytes.
** Defaults to 65536 when the directive is absent or invalid.
*/
static int extractMaxBodySize(std::string input) {
    size_t loc_pos = input.find("location");
    if (loc_pos != std::string::npos) {
        input = input.substr(0, loc_pos);
    }

    const std::string target = "client_max_body_size";
    size_t kw_pos     = input.find(target);
    std::string size_str = "";

    while (kw_pos != std::string::npos) {
        bool before_ok = (kw_pos == 0 || std::isspace(input[kw_pos - 1]));
        bool after_ok  = (kw_pos + target.length() < input.length() &&
                          std::isspace(input[kw_pos + target.length()]));

        if (before_ok && after_ok) {
            size_t val_start = kw_pos + target.length();
            size_t sc_pos    = input.find(';', val_start);

            if (sc_pos != std::string::npos) {
                size_str = stripWhitespace(input.substr(val_start, sc_pos - val_start));
                break;
            }
        }
        kw_pos = input.find(target, kw_pos + 1);
    }

    if (size_str.empty()) {
        return 65536;
    }

    for (size_t i = 0; i < size_str.length(); ++i) {
        if (!std::isdigit(size_str[i])) return 65536;
    }

    return std::atoi(size_str.c_str());
}

/* Extracts the global "index" directive value. */
static std::string extractIndexFile(std::string input) {
    size_t loc_pos = input.find("location");
    if (loc_pos != std::string::npos) {
        input = input.substr(0, loc_pos);
    }

    const std::string target = "index";
    size_t kw_pos        = input.find(target);
    std::string idx_file = "";

    while (kw_pos != std::string::npos) {
        bool before_ok = (kw_pos == 0 || std::isspace(input[kw_pos - 1]));
        bool after_ok  = (kw_pos + target.length() < input.length() &&
                          std::isspace(input[kw_pos + target.length()]));

        if (before_ok && after_ok) {
            size_t val_start = kw_pos + target.length();
            size_t sc_pos    = input.find(';', val_start);

            if (sc_pos != std::string::npos) {
                idx_file = stripWhitespace(input.substr(val_start, sc_pos - val_start));
                break;
            }
        }
        kw_pos = input.find(target, kw_pos + 1);
    }

    return idx_file;
}

/*
** Collects all "error_page" directives (there may be several).
** Returns them as a single space-separated string.
*/
static std::string extractErrorPages(std::string input) {
    size_t loc_pos = input.find("location");
    if (loc_pos != std::string::npos) {
        input = input.substr(0, loc_pos);
    }

    std::string accumulated;
    const std::string target = "error_page";
    size_t kw_pos = input.find(target);

    while (kw_pos != std::string::npos) {
        bool before_ok = (kw_pos == 0 || std::isspace(input[kw_pos - 1]));
        bool after_ok  = (kw_pos + target.length() < input.length() &&
                          std::isspace(input[kw_pos + target.length()]));

        if (before_ok && after_ok) {
            size_t val_start = kw_pos + target.length();
            size_t sc_pos    = input.find(";", val_start);

            if (sc_pos != std::string::npos) {
                accumulated += stripWhitespace(input.substr(val_start, sc_pos - val_start)) + " ";
                kw_pos = input.find(target, sc_pos);
            } else {
                break;
            }
        } else {
            kw_pos = input.find(target, kw_pos + 1);
        }
    }
    return stripWhitespace(accumulated);
}

/*
** Collects all "location { ... }" blocks from a server block.
** Returns them concatenated with '|' as separator.
*/
static std::string extractLocationBlocks(std::string input) {
    std::string accumulated;
    const std::string target = "location";
    size_t kw_pos = input.find(target);

    while (kw_pos != std::string::npos) {
        bool before_ok = (kw_pos == 0 || std::isspace(input[kw_pos - 1]));
        bool after_ok  = (kw_pos + target.length() < input.length() &&
                          std::isspace(input[kw_pos + target.length()]));

        if (before_ok && after_ok) {
            size_t close_pos = input.find("}", kw_pos);

            if (close_pos != std::string::npos) {
                accumulated += stripWhitespace(input.substr(kw_pos, close_pos - kw_pos + 1)) + "|";
                kw_pos = input.find(target, close_pos);
            } else {
                break;
            }
        } else {
            kw_pos = input.find(target, kw_pos + 1);
        }
    }
    return stripWhitespace(accumulated);
}

// ---------- ConfigParser implementation ----------

std::string ConfigParser::parseConfigFile(std::string file) {
    std::ifstream cfg_stream(file.c_str());
    if (!cfg_stream.is_open())
    {
        return "";
    }
    std::string raw_content((std::istreambuf_iterator<char>(cfg_stream)), std::istreambuf_iterator<char>());
    stripCommentedLines(raw_content);
    if (raw_content.empty())
    {
        std::cerr << "Error: config file is empty" << std::endl;
        return "";
    }
    if (!checkBraceBalance(raw_content))
    {
        std::cerr << "Error: config file has unbalanced braces" << std::endl;
        return "";
    }

    size_t scan_pos  = 0;
    size_t sv_found  = 0;
    std::string sv_block;
    while ((sv_block = extractServerBlock(raw_content, scan_pos, sv_found)) != "")
    {
        std::string sv_name   = extractServerName(sv_block);
        std::string sv_host   = extractHostAddr(sv_block);
        int         sv_port   = extractPortNum(sv_block);
        std::string sv_root   = extractRootDir(sv_block);
        int         sv_body   = extractMaxBodySize(sv_block);
        std::string sv_index  = extractIndexFile(sv_block);
        std::string sv_errors = extractErrorPages(sv_block);
        std::string sv_locs   = extractLocationBlocks(sv_block);
        _srv_list.push_back(Server(sv_name, sv_host, sv_port, sv_root, sv_body, sv_index, sv_errors, sv_locs));
    }
    return file;
}

// ---------- Constructors ----------

ConfigParser::ConfigParser(const ConfigParser &other)
: _srv_list(other._srv_list), _cfg_path(other._cfg_path)
{
}

ConfigParser::ConfigParser(std::string absConfigPath)
: _cfg_path(parseConfigFile(absConfigPath))
{
}

ConfigParser &ConfigParser::operator=(const ConfigParser &other) {
    this->~ConfigParser();
    ::new(this) ConfigParser(other);
    return (*this);
}

ConfigParser::~ConfigParser() {
}

// ---------- Getters ----------

std::vector<Server> ConfigParser::getServerVector() {
    return this->_srv_list;
}

std::string ConfigParser::getConfigFile() const {
    return this->_cfg_path;
}
