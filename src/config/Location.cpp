#include "Location.hpp"

std::ostream	&operator << (std::ostream &stream, const Location &location) {
    const std::map<int, std::string>& map = location.getRedirectPageMap();

    stream << \
    "      Location:   " << location.getLocation() << std::endl << \
    "                  " << location.getRoot() << " (root)" << std::endl << \
    "                  " << location.getIndex() << " (index)" << std::endl << \
    "                  " << location.getAutoindex() << " (autoindex)" << std::endl << \
    "                  " << location.getMethods() << " (allowMethods)" << std::endl << \
    "                  " << location.getReturn() << " (return)" << std::endl << \
    "                  " << location.getTryFiles() << " (tryFiles)" << std::endl << \
    "                  " << location.getCgiPath() << " (cgiPath)" << std::endl << \
    "                  " << location.getCgiExt() << " (cgiExtension)" << std::endl;
    for (std::map<int, std::string>::const_iterator it = map.begin(); it != map.end(); ++it)
    {
        stream << \
    "                  " << it->first << " handled by " << it->second << "(redirection)" << std::endl;
    }

    return (stream);
}

void Location::setRedirects(const std::string& redirects) {
    std::istringstream iss(redirects);
    int key;
    std::string value;
    while (iss >> key >> value)
        this->_redirect[key] = value;
}

Location::Location(const Location &loc)
:_location(loc._location),_root(loc._root),_index(loc._index),_autoindex(loc._autoindex),_allowMethods(loc._allowMethods),_redirect(loc._redirect),_return(loc._return),_try_files(loc._try_files),_cgi_path(loc._cgi_path),_cgi_ext(loc._cgi_ext),_maxBodySize(loc._maxBodySize)
{
}

Location::Location(std::string location, std::string root, bool autoindex,std::string returnStr, std::string tryFiles, std::string cgiPath, std::string cgiExt,std::string redirects,std::string indice, std::string methods, int maxBodySize)
:_location(location), _root(root), _index(indice), _autoindex(autoindex), _allowMethods(methods), _return(returnStr), _try_files(tryFiles), _cgi_path(cgiPath), _cgi_ext(cgiExt), _maxBodySize(maxBodySize)
{
    this->setRedirects(redirects);
}

Location	&Location::operator = (const Location &location) {
    this->~Location();
    ::new(this) Location(location);
    return (*this);
}

Location::~Location() {
}

std::string	Location::getLocation() const
{
    return this->_location;
}

std::string	Location::getRoot() const
{
    return this->_root;
}

std::string	Location::getIndex() const
{
    return this->_index;
}

std::string	Location::getMethods() const
{
    return this->_allowMethods;
}

bool Location::getAutoindex() const
{
    return _autoindex;
}

std::string	Location::getReturn() const
{
    return this->_return;
}

std::string	Location::getTryFiles() const
{
    return this->_try_files;
}

std::string	Location::getCgiPath() const
{
    return this->_cgi_path;
}

std::string	Location::getCgiExt() const
{
    return this->_cgi_ext;
}

const std::map<int, std::string>& Location::getRedirectPageMap() const
{
    return _redirect;
}

std::string	Location::getRedirectPage(int code) const
{
    std::map<int, std::string>::const_iterator it = _redirect.find(code);
    if (it != _redirect.end())
        return it->second;
    else
        return "";
}

int Location::getMaxBodySize() const
{
    return _maxBodySize;
}
