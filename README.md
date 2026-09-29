*This project has been created as part of the 42 curriculum by gustgonz, csalaun.*

# Webserv — HTTP Server in C++98

🇫🇷 [Version française](README.fr.md)

> *This is when you finally understand why URLs start with HTTP*

A fully functional HTTP/1.1 server written from scratch in **C++98**, built as part of the 42 School curriculum. The server handles concurrent connections using a single non-blocking I/O multiplexing call (`select()`), supports multiple virtual servers, location-based routing, file uploads (including multipart/form-data), and asynchronous CGI execution.

---

## Table of Contents

- [Description](#description)
- [Instructions](#instructions)
  - [Requirements](#requirements)
  - [Build](#build)
  - [Usage](#usage)
- [Configuration File](#configuration-file)
  - [Server Block Directives](#server-block-directives)
  - [Location Block Directives](#location-block-directives)
  - [Full Example](#full-example)
- [Features](#features)
- [HTTP Methods](#http-methods)
- [CGI Support](#cgi-support)
- [Status Codes](#status-codes)
- [Project Architecture](#project-architecture)
- [Testing](#testing)
- [Resources](#resources)

---

## Description

Webserv is an HTTP/1.1-compliant web server implemented entirely in C++98, without any external libraries. It accepts a configuration file inspired by NGINX syntax and serves static websites, handles file uploads, executes CGI scripts asynchronously, and manages multiple virtual servers simultaneously — all within a single-process, single-thread, non-blocking event loop driven by `select()`.

The project demonstrates mastery of POSIX socket programming, I/O multiplexing, the HTTP protocol, CGI, and process management in C++.

---

## Instructions

### Requirements

- **Compiler:** `g++` with C++98 standard
- **OS:** Linux or macOS
- **No external libraries** — pure C++98 and POSIX system calls only

### Build

```bash
make          # build the webserv binary
make clean    # remove object files
make fclean   # remove object files and binary
make re       # full rebuild
```

Compilation flags: `-Wall -Wextra -Werror -std=c++98 -g -O3`

### Usage

```bash
./webserv [configuration_file]
```

The argument must be a file with a `.conf` extension. If no argument is given or the file is invalid, the program exits with an error message.

**Examples:**

```bash
# Single server on port 8001
./webserv config/default.conf

# Two servers: port 8001 and port 8042
./webserv config/2default.conf

# Config for the 42 CGI tester
./webserv config/tester.conf
```

**Signal handling:**

| Signal | Behaviour |
|--------|-----------|
| `SIGINT` (Ctrl+C) | Graceful shutdown |
| `SIGTERM` | Graceful shutdown |
| `SIGPIPE` | Ignored — broken pipe on a client socket does not crash the server |

---

## Configuration File

The configuration syntax is inspired by NGINX. The file is composed of one or more `server` blocks, each optionally containing `location` blocks. Comments start with `#` and are ignored.

> You can take inspiration from the `server` section of the NGINX configuration file.

### Server Block Directives

| Directive | Description | Default |
|-----------|-------------|---------|
| `server_name` | Name identifier for the server | — |
| `host` | IP address to bind | `127.0.0.1` |
| `listen` | Port to listen on | — |
| `root` | Default root directory for file serving | — |
| `index` | Default index file for directory requests | — |
| `client_max_body_size` | Maximum allowed body size in bytes | `65536` |
| `error_page <code> <path>` | Custom error page for an HTTP status code | built-in default |

### Location Block Directives

| Directive | Description |
|-----------|-------------|
| `root` | Overrides the server root for this location |
| `index` | Overrides the server index for this location |
| `allow_methods` | Space-separated list of allowed HTTP methods (`GET POST DELETE`) |
| `autoindex` | Enable directory listing when no index file exists (`on` / `off`) |
| `client_max_body_size` | Per-location body size limit (overrides server-level value) |
| `redirect <code> <url>` | Redirect requests to another URL (e.g. `redirect 301 /new-path`) |
| `cgi_path` | Path to the CGI interpreter (e.g. `/bin/bash`, `./cgi_tester`) |
| `cgi_ext` | File extension handled as CGI (e.g. `.sh`, `.bla`) |
| `try_files` | File to attempt before falling back |

### Full Example

```nginx
server {
    server_name default_server;
    host 127.0.0.1;
    listen 8001;
    root ./www;
    client_max_body_size 1000000;

    error_page 403 /error_pages/403.html;
    error_page 404 /error_pages/404.html;
    error_page 500 /error_pages/500.html;

    # Serve static files
    location / {
        root ./www;
        index index.html;
        allow_methods GET;
    }

    # Upload and list files (POST saves body to disk)
    location /upload {
        root ./www/upload;
        allow_methods GET POST DELETE;
        autoindex on;
    }

    # Execute shell CGI scripts
    location /cgi-bin {
        root ./www/cgi-bin;
        allow_methods GET POST;
        cgi_path /bin/bash;
        cgi_ext .sh;
    }

    # External redirect
    location /git {
        redirect 301 http://github.com;
    }
}

server {
    listen 8042;
    server_name localhost;
    host 127.0.0.1;
    root ./www_server2;
    client_max_body_size 3000000;
    index index.html;

    error_page 403 /error_pages/403.html;
    error_page 404 /error_pages/404.html;
    error_page 500 /error_pages/500.html;

    location / {
        root ./www_server2;
        allow_methods DELETE POST GET;
        autoindex off;
    }

    location /db {
        root ./www_server2;
        allow_methods GET POST DELETE;
        autoindex on;
    }

    location /tours {
        root ./www_server2/tours;
        autoindex on;
        index tours1.html;
        allow_methods GET POST DELETE;
    }

    location /red {
        redirect 301 /tours;
    }

    location /cgi-bin {
        root ./www_server2/cgi-bin;
        allow_methods GET POST DELETE;
        cgi_path /bin/bash;
        cgi_ext .sh;
    }
}
```

---

## Features

- **Non-blocking I/O** — all sockets run in non-blocking mode; a single `select()` call drives all I/O (client sockets, listening sockets, and CGI pipes)
- **Multiple virtual servers** — serve different content on different ports from a single process
- **Location-based routing** — longest-prefix match determines which location block handles each request
- **Static file serving** — serves HTML, CSS, images, and any file type with correct MIME type
- **File uploads via POST** — supports both raw body uploads and `multipart/form-data` (browser forms)
- **File deletion via DELETE** — clients can remove uploaded resources
- **Asynchronous CGI execution** — scripts are executed via `fork()` + `execve()` with non-blocking pipes managed by the event loop; a 60-second timeout prevents the server from hanging if a CGI stalls
- **Directory listing (autoindex)** — generated in C++ (`opendir()` / `readdir()`) when `autoindex on` is set and no index file is present
- **HTTP redirects** — configurable 301 redirections per location (internal paths or external URLs)
- **Custom error pages** — per-status-code error pages configurable in the config file; built-in defaults always available
- **Keep-alive connections** — persistent connections via `Connection: keep-alive`
- **Graceful shutdown** — SIGINT and SIGTERM are caught; SIGPIPE is ignored
- **`SO_REUSEADDR`** — immediate port reuse after server restart

---

## HTTP Methods

| Method | Behaviour |
|--------|-----------|
| `GET` | Serves a static file, executes a CGI script, or generates a directory listing |
| `POST` | Saves the request body to disk (raw or multipart); returns `201 Created` |
| `DELETE` | Removes the file corresponding to the resource; returns `200 OK` |

Any other method returns `501 Not Implemented`. Methods not listed in `allow_methods` for the matched location return `405 Method Not Allowed`.

---

## CGI Support

CGI scripts are executed when a request targets a file whose extension matches `cgi_ext` in the location block.

**Supported CGI types (examples):**

| Extension | Interpreter |
|-----------|-------------|
| `.sh` | `/bin/bash` |
| `.bla` | `./cgi_tester` (42 tester binary) |
| Any | Configurable via `cgi_path` + `cgi_ext` |

**Asynchronous execution flow:**

1. `pipe()` is created for stdin (server → CGI) and stdout (CGI → server).
2. `fork()` spawns the child process.
3. Child: redirects stdin/stdout to pipes, sets CGI environment variables, calls `execve()`.
4. Parent: **returns immediately** — the event loop monitors CGI pipes via `select()`.
5. Event loop writes the request body to CGI stdin asynchronously, then reads the output until EOF.
6. If the CGI does not complete within **60 seconds**, the child is killed with `SIGKILL` and a `504 Gateway Timeout` is returned.
7. The CGI output (headers + body) is parsed and forwarded to the client as a valid HTTP response.

**CGI environment variables:**

| Variable | Value |
|----------|-------|
| `REQUEST_METHOD` | HTTP method (`GET`, `POST`, etc.) |
| `QUERY_STRING` | Query string from the URI (after `?`) |
| `CONTENT_LENGTH` | Size of the request body |
| `CONTENT_TYPE` | Content-Type header from the request |
| `PATH_INFO` | Full request URI |
| `SCRIPT_NAME` | Request URI |
| `SERVER_NAME` | Server name from config |
| `SERVER_PORT` | Listening port |
| `SERVER_PROTOCOL` | `HTTP/1.1` |
| `GATEWAY_INTERFACE` | `CGI/1.1` |

**Autoindex** (directory listing) is not a CGI: the HTML listing is built directly in `Response.cpp` with `opendir()` / `readdir()` when `autoindex on` is set and no index file exists.

---

## Status Codes

| Code | Meaning | When returned |
|------|---------|---------------|
| `200 OK` | Success | Static file served, DELETE succeeded |
| `201 Created` | Resource created | POST upload succeeded |
| `301 Moved Permanently` | Redirect | `redirect` directive in location |
| `403 Forbidden` | Access denied | Directory without index and autoindex off |
| `404 Not Found` | Resource missing | File does not exist |
| `405 Method Not Allowed` | Method not permitted | Method not in `allow_methods` |
| `413 Content Too Large` | Body too big | Body exceeds `client_max_body_size` |
| `500 Internal Server Error` | Server-side failure | CGI failed, file write error |
| `501 Not Implemented` | Unknown method | Method not supported by the server |
| `504 Gateway Timeout` | CGI timed out | CGI did not complete within 60 seconds |

---

## Project Architecture

```
webserv/
├── src/
│   ├── main.cpp                  # Entry point: parse config, set signals, start event loop
│   ├── server/
│   │   └── Server.cpp            # Event loop (select), socket management, CGI lifecycle
│   ├── config/
│   │   ├── ConfigParser.cpp      # Parses .conf file into Server/Location objects
│   │   └── Location.cpp          # Location block data model
│   └── http/
│       ├── Request.cpp           # HTTP request parser (method, headers, body, URI, multipart)
│       └── Response.cpp          # Response builder (GET/POST/DELETE/CGI/redirect)
├── include/
│   ├── Server.hpp                # Server class: config + socket + event loop entry point
│   ├── ConfigParser.hpp          # Config file parser
│   ├── Request.hpp               # Parsed HTTP request structure
│   ├── Response.hpp              # Response builder + async CGI launcher
│   ├── Location.hpp              # Location block structure
│   └── CgiState.hpp              # In-flight CGI process state (pid, pipes, buffers, timeout)
├── config/
│   ├── default.conf              # Single server on port 8001
│   ├── 2default.conf             # Two servers: ports 8001 and 8042
│   ├── tester.conf               # Config for the 42 CGI tester (YoupiBanane)
│   └── empty.conf
├── www/                          # Static files for server 1 (port 8001)
│   ├── index.html
│   ├── cgi-bin/
│   │   ├── script.sh             # Example CGI script
│   │   └── error.sh, infinite.sh # CGI error / timeout test scripts
│   ├── upload/                   # Upload destination for POST requests
│   ├── db/                       # Alternative upload destination
│   └── error_pages/              # Custom 403, 404, 500 pages
├── www_server2/                  # Static files for server 2 (port 8042)
│   ├── index.html
│   ├── cgi-bin/
│   ├── db/
│   ├── error_pages/
│   └── tours/
└── YoupiBanane/                  # Test directory used by the 42 CGI tester
```

### Data Flow

```
1. main()           reads .conf file, sets up signal handlers
2. ConfigParser     builds a vector of Server objects (one per server block)
3. Server::run()    starts the event loop:
     - select() waits for I/O readiness on all fds simultaneously
     - Listening fd ready   → accept() new client, set O_NONBLOCK, add to watch set
     - Client fd readable   → recv() bytes → accumulate → detect complete request
                              → if CGI: startCGIAsync() → CgiState stored, return
                              → if not CGI: build Response → queue for sending
     - CGI stdin fd ready   → write request body chunk to CGI process
     - CGI stdout fd ready  → read CGI output → accumulate until EOF
     - CGI timeout (60s)    → kill child with SIGKILL, send 504 error
     - Client fd writable   → send() buffered response
     - Connection closed    → FD_CLR(), close fd, erase from client map
```

---

## Testing

### Basic requests

```bash
# GET static file
curl http://127.0.0.1:8001/

# POST — raw body upload (saved as upload/myfile.txt)
curl -X POST -d "hello world" http://127.0.0.1:8001/upload/myfile

# POST — multipart form-data (browser-style file upload)
curl -F "file=@/path/to/file.txt" http://127.0.0.1:8001/upload/

# GET uploaded file
curl http://127.0.0.1:8001/upload/myfile.txt

# DELETE uploaded file
curl -X DELETE http://127.0.0.1:8001/upload/myfile.txt

# 301 redirect
curl -v http://127.0.0.1:8001/git

# Method not allowed on this location → 405 ("/" only allows GET)
curl -X POST -d x http://127.0.0.1:8001/

# Unknown method → 501
curl -X PATCH http://127.0.0.1:8001/

# CGI script
curl http://127.0.0.1:8001/cgi-bin/script.sh

# Body too large → 413
curl -X POST --data-binary @/dev/urandom http://127.0.0.1:8001/upload/ --max-time 2
```

### Two-server setup

```bash
./webserv config/2default.conf

# Server 1 — port 8001
curl http://127.0.0.1:8001/

# Server 2 — port 8042 (redirect /red → /tours)
curl -L http://127.0.0.1:8042/red
```

### 42 CGI tester

```bash
./webserv config/tester.conf
# Then run the provided cgi_tester binary
```

### Browser test

Open `http://127.0.0.1:8001/` in any browser. Static HTML, CSS, and images should load correctly. Directory listing appears when `autoindex on` is set and no index file is found.

### Stress test

```bash
# Siege benchmark — availability must be > 99.5%
siege -b -c 10 -r 100 http://127.0.0.1:8001/

# Increase concurrency
siege -b -c 25 -r 200 http://127.0.0.1:8001/

# Run for a fixed duration
siege -b -c 10 -t 1M http://127.0.0.1:8001/

# Monitor memory growth while siege runs (should stabilize, not grow indefinitely)
watch -n 1 'cat /proc/$(pgrep webserv)/status | grep -E "VmRSS|VmSize"'

# Apache Bench alternative
ab -n 10000 -c 100 http://127.0.0.1:8001/
```

The server must remain available and not crash under concurrent load. Availability must stay above **99.5%**.

### Raw HTTP (telnet)

```bash
telnet 127.0.0.1 8001
GET / HTTP/1.1
Host: localhost

```

---

## Resources

**HTTP Protocol**
- [RFC 7230 — HTTP/1.1 Message Syntax and Routing](https://datatracker.ietf.org/doc/html/rfc7230)
- [RFC 7231 — HTTP/1.1 Semantics and Content](https://datatracker.ietf.org/doc/html/rfc7231)
- [RFC 3875 — The Common Gateway Interface (CGI/1.1)](https://datatracker.ietf.org/doc/html/rfc3875)

**NGINX reference**
- [NGINX `server` block documentation](https://nginx.org/en/docs/http/ngx_http_core_module.html)

**Socket programming**
- Beej's Guide to Network Programming — comprehensive POSIX sockets reference
- `man 2 select`, `man 2 poll`, `man 7 socket`, `man 2 accept`

**How AI was used in this project**

AI tools (Claude) were used as a pair-programming assistant during development:
- Debugging the asynchronous CGI state machine and pipe lifecycle
- Understanding edge cases in HTTP request parsing (chunked encoding, multipart/form-data boundaries)
- Reviewing signal handling and non-blocking I/O patterns
- Explaining RFC sections related to response status codes and header formatting

All AI-generated suggestions were reviewed, understood, and tested before integration. No code was copied without full comprehension.

---

## Authors

| Person | Domain |
|--------|--------|
| **gustgonz** | Networking, sockets, event loop (`Server`, `ConfigParser`) |
| **csalaun** | HTTP protocol, responses, CGI (`Request`, `Response`, `Location`) |

---

*42 School — Webserv project*
