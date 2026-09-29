#ifndef RESPONSE_HPP
# define RESPONSE_HPP

#include "Request.hpp"
#include "Server.hpp"
#include "CgiState.hpp"

#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <utility>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>

class Response {
private:
    std::string _statusCode;
    std::string _body;
    std::string _contentType;
    std::string _extraHeaders; // headers forwarded from CGI response
    bool   _isHead;
    size_t _headContentLength;

    static std::string replaceDoubleSlashes(const std::string& str);
    static int findLocation(std::vector<Location> locations, std::string toMatch);
    std::string readFromPipe(int fileDescriptor);
    void startCGIscript(const std::string& scriptPath, const std::string& requestData, const Request& req, const Server& srv, const std::string& interpreter = "/bin/bash");
    bool isCgiRequest(const std::string& target) const;

    // Manejadores por tipo de recurso/método
    void handleDirectory(const std::string& path, int locIdx, const Request& req, const Server& srv);
    void handleGET(const std::string& path, const std::string& target, const std::string& allowedMethods, const Request& req, const Server& srv);
    void handlePOST(const std::string& path, const std::string& body, const std::string& allowedMethods, const Server& srv, const Request& req);
    void handleMultipart(const std::string& path, const std::string& body, const std::string& boundary);
    void handleDELETE(const std::string& path, const std::string& allowedMethods, const Server& srv);

    Response(); // constructor por defecto para uso interno (buildFromCGIOutput)

public:
    static std::string readFromFile(std::string path);

    Response(const Request& req, Server& srv);
    ~Response();

    std::string build() const;
    std::string getStatusCode() const;
    std::string getBody() const;

    // CGI asíncrono: detecta si el request necesita CGI, hace fork+pipes
    // y retorna inmediatamente. El event loop maneja los pipes desde aquí.
    // Si pid == -1: no era CGI → tratar con Response() normal.
    static CgiState startCGIAsync(const Request& req, Server& srv,
                                   size_t body_remaining = 0,
                                   size_t full_content_length = 0);

    // Convierte la salida cruda del CGI en un string HTTP/1.1 completo.
    // Llamar cuando stdout_fd haya llegado a EOF.
    static std::string buildFromCGIOutput(const std::string& cgiOut,
                                          const Request& req, Server& srv);
};

#endif
