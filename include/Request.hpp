#ifndef REQUEST_HPP
# define REQUEST_HPP

#include <iostream>
#include <string>
#include <vector>
#include <utility>
#include <sstream>
#include <map>

class Request {
private:
    Request();

    std::string		_requestCopy;
    std::string		_reqType;
    std::string		_reqTarget;
    std::string  	_version;
    std::string		_headers;
    std::string		_body;
    std::string 	_path;
    std::vector<std::pair<std::string, std::string> >	_queryParams;
    std::string		_connectionType;
    std::string		_contentType;  // "multipart/form-data", "text/plain", etc.
    std::string		_boundary;     // valor del boundary en multipart/form-data

public:
    Request(std::string request);
    ~Request();

    std::string getMethod() const;
    std::string getTarget() const;
    std::string getVersion() const;
    std::string getHeaders() const;
    std::string getBody() const;
    std::string getPath() const;
    void setPath(std::string path);
    std::string getConnectionType() const;
    void setConnectionType(std::string type);
    std::string getContentType() const;
    std::string getBoundary() const;
    const std::vector<std::pair<std::string, std::string> >& getQueryParams() const;
    int parseURI();
};

#endif
