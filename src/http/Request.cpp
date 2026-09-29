#include "Request.hpp"

Request::Request(std::string request)
{
    if (request.size() == 0)
        return;

    std::istringstream iss(request);
    std::string line;

    std::getline(iss, line);
    std::istringstream firstLineStream(line);
    firstLineStream >> _reqType >> _reqTarget >> _version;

    // El loop lee los headers uno a uno.
    // Condición de parada: línea vacía "" O línea con solo "\r".
    // ¿Por qué "\r"? HTTP usa CRLF (\r\n) como separador de líneas.
    // getline() extrae hasta el \n y lo descarta, pero deja el \r.
    // La línea en blanco que separa headers de body es \r\n → getline la lee como "\r".
    // Sin este chequeo, el loop continuaría leyendo el body del multipart,
    // encontraría "Content-Type: text/plain" de la primera parte y sobreescribiría
    // el _contentType correcto ("multipart/form-data") con "text/plain".
    while (std::getline(iss, line) && line.compare(std::string("")) && line != "\r")
    {
        // Detectar el tipo de conexión (keep-alive o close)
        if (line.find("Connection:") != std::string::npos)
        {
            std::string connVal = line.substr(line.find_first_of(" ") + 1);
            if (!connVal.empty() && connVal[connVal.size() - 1] == '\r')
                connVal.erase(connVal.size() - 1);
            setConnectionType(connVal);
            std::cout << "\033[38;5;177m" << getConnectionType() << "\033[0m" << std::endl;
        }

        // Detectar el Content-Type para saber si es multipart/form-data
        // Ejemplo de header: "Content-Type: multipart/form-data; boundary=----WebKit123"
        if (line.find("Content-Type:") != std::string::npos)
        {
            // Extraer el valor completo después de "Content-Type: "
            std::string val = line.substr(line.find(":") + 2);

            // Eliminar el \r del final si existe (artefacto del protocolo HTTP)
            if (!val.empty() && val[val.size() - 1] == '\r')
                val.erase(val.size() - 1);

            // Comprobar si es una petición multipart (subida de archivo)
            if (val.find("multipart/form-data") != std::string::npos)
            {
                _contentType = "multipart/form-data";

                // Extraer el boundary: es el separador entre partes del multipart
                // Ejemplo: "boundary=----WebKitFormBoundary7xyz"
                size_t bpos = val.find("boundary=");
                if (bpos != std::string::npos)
                {
                    _boundary = val.substr(bpos + 9); // 9 = longitud de "boundary="

                    // Limpiar posibles espacios o \r residuales
                    if (!_boundary.empty() && _boundary[_boundary.size() - 1] == '\r')
                        _boundary.erase(_boundary.size() - 1);
                }
            }
            else
            {
                // Para otros tipos (text/plain, application/json, etc.)
                _contentType = val;
            }
        }

        if (!line.empty() && line[line.size() - 1] == '\r')
            line.erase(line.size() - 1);
        _headers += line + "\r\n";
    }

    std::size_t idx = request.find(std::string("\r\n\r\n"));
    if (idx != std::string::npos)
        _body = request.substr(idx + 4);
    else
        _body = "";

    parseURI();
}

Request::~Request() {}

std::string Request::getMethod() const { return _reqType; }
std::string Request::getTarget() const { return _reqTarget; }
std::string Request::getVersion() const { return _version; }
std::string Request::getHeaders() const { return _headers; }
std::string Request::getBody() const { return _body; }
std::string Request::getPath() const { return _path; }

void Request::setPath(std::string path) { _path = path; }

std::string Request::getConnectionType() const { return _connectionType; }
void Request::setConnectionType(std::string type) { _connectionType = type; }
std::string Request::getContentType() const { return _contentType; }
std::string Request::getBoundary() const { return _boundary; }

const std::vector<std::pair<std::string, std::string> >& Request::getQueryParams() const {
    return _queryParams;
}

int Request::parseURI()
{
    size_t pos = _reqTarget.find('?');
    if (pos != std::string::npos)
    {
        _path = _reqTarget.substr(0, pos);
        std::string queryString = _reqTarget.substr(pos + 1);
        std::istringstream iss(queryString);
        std::string token;
        while (std::getline(iss, token, '&'))
        {
            size_t eqPos = token.find('=');
            if (eqPos != std::string::npos)
            {
                std::string key = token.substr(0, eqPos);
                std::string value = token.substr(eqPos + 1);
                _queryParams.push_back(std::make_pair(key, value));
            }
        }
        return 1;
    } else {
        _path = _reqTarget;
        return 0;
    }
}
