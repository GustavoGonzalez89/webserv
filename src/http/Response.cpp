#include "Response.hpp"
#include <ctime>

// ---------- Static helpers ----------

std::string Response::readFromFile(std::string path) {
    std::ifstream file(path.c_str());
    std::string file_content;
    char ch;
    while (file.get(ch)) {
        file_content += ch;
    }
    file.close();
    return file_content;
}

std::string Response::replaceDoubleSlashes(const std::string& str) {
    std::string result = str;
    std::string::size_type pos = 0; // esto es un unsigned y en la linea de abajo tenemos una comparacion
    while ((pos = result.find("//", pos)) != std::string::npos) {
        result.replace(pos, 2, "/");
        pos += 1;
    }
    return result;
}

int Response::findLocation(std::vector<Location> locations, std::string toMatch) {
    toMatch += "/" + toMatch;
    int locLength = 0;
    int bestLoc = -1;
    for (size_t i = 0; i < locations.size(); i++) {
        std::string loc = locations[i].getLocation();
        if (loc.substr(0, loc.size()) == toMatch.substr(0, loc.size()) && static_cast<int>(loc.size()) > locLength) {
            locLength = static_cast<int>(loc.size());
            bestLoc = static_cast<int>(i);
        }
    }
    return bestLoc;
}

bool Response::isCgiRequest(const std::string& target) const {
    if (target.size() <= 1)
        return false;
    std::string ext = target.substr(target.find_last_of(".") + 1);
    size_t spacePos = ext.find_first_of(" ");
    if (spacePos != std::string::npos)
        ext = ext.substr(0, spacePos);
    return (ext == "sh");
}

// std::string Response::readFromPipe(int fileDescriptor) {
//     std::string output;
//     char buf[1024];
//     int bytesRead;
//     while ((bytesRead = read(fileDescriptor, buf, sizeof(buf) - 1)) > 0) {
//         buf[bytesRead] = 0;
//         output += buf;
//     }
//     return output;
// }

// buildCGIEnvVec — Construye el entorno CGI/1.1 como vector de strings "KEY=VALUE".
// Se pasa directamente a execve() como envp, sin tocar el entorno del proceso padre.
static std::vector<std::string> buildCGIEnvVec(const Request& req, const Server& srv,
                                                const std::string& requestData,
                                                size_t content_len = 0)
{
    std::vector<std::string> env;

    env.push_back("REQUEST_METHOD="    + req.getMethod());
    env.push_back("SERVER_NAME="       + srv.getServerName());
    env.push_back("SERVER_PROTOCOL="   + req.getVersion());
    env.push_back("GATEWAY_INTERFACE=CGI/1.1");
    env.push_back("PATH_INFO="         + req.getTarget());
    env.push_back("REQUEST_URI="       + req.getTarget());
    env.push_back("SCRIPT_NAME="       + req.getTarget());

    // QUERY_STRING: parte de la URI después del '?'
    std::string query_str;
    size_t qpos = req.getTarget().find('?');
    if (qpos != std::string::npos)
        query_str = req.getTarget().substr(qpos + 1);
    env.push_back("QUERY_STRING=" + query_str);

    // SERVER_PORT como string (C++98 no tiene std::to_string)
    std::ostringstream port_ss;
    port_ss << srv.getListen();
    env.push_back("SERVER_PORT=" + port_ss.str());

    // CONTENT_LENGTH: usar full_content_length si fue pasado (early CGI start),
    // sino el tamaño real del cuerpo de la petición.
    std::ostringstream cl_ss;
    cl_ss << (content_len > 0 ? content_len : requestData.size());
    env.push_back("CONTENT_LENGTH=" + cl_ss.str());

    // CONTENT_TYPE: extraído de los headers de la request.
    std::string ct_val;
    std::istringstream ct_stream(req.getHeaders());
    std::string ct_line;
    while (std::getline(ct_stream, ct_line)) {
        if (ct_line.size() >= 13 &&
            ct_line.substr(0, 13) == "Content-Type:") {
            ct_val = ct_line.substr(13);
            size_t s = ct_val.find_first_not_of(" \t\r");
            if (s != std::string::npos) ct_val = ct_val.substr(s);
            size_t e = ct_val.find_last_not_of(" \t\r\n");
            if (e != std::string::npos) ct_val = ct_val.substr(0, e + 1);
            break;
        }
    }
    env.push_back("CONTENT_TYPE=" + ct_val);

    // HTTP_* variables: reenvío de los headers HTTP de la request al CGI.
    // El estándar CGI/1.1 exige que cada header "X-Foo-Bar: valor" se exponga
    // como la variable de entorno "HTTP_X_FOO_BAR=valor" (mayúsculas, '-' → '_').
    // Sin esto, headers como "X-Secret-Header-For-Test" no llegan al script CGI.
    std::istringstream hstream(req.getHeaders());
    std::string hline;
    while (std::getline(hstream, hline)) {
        // Eliminar TODOS los \r del final.
        // _headers concatena líneas como "line + \r\n" donde line ya tiene \r
        // (artefacto de getline con CRLF), resultando en "Header: valor\r\r\n".
        // Al volver a parsear con getline, quedan dos \r → el if solo borra uno.
        // El \r residual en el valor contamina env vars como HTTP_HOST, haciendo
        // que Go (net/http/cgi) falle al construir la URL: "http://host\r/path".
        while (!hline.empty() && hline[hline.size() - 1] == '\r')
            hline.erase(hline.size() - 1);
        if (hline.empty())
            continue;

        // Separar nombre y valor en "Nombre: valor"
        size_t colon = hline.find(':');
        if (colon == std::string::npos)
            continue;
        std::string name  = hline.substr(0, colon);
        std::string value = hline.substr(colon + 1);

        // Eliminar espacio inicial del valor
        size_t vstart = value.find_first_not_of(' ');
        if (vstart != std::string::npos)
            value = value.substr(vstart);

        // Content-Length no se reenvía como HTTP_CONTENT_LENGTH porque el
        // estándar CGI/1.1 ya lo expone a través de la variable CONTENT_LENGTH.
        if (name == "Content-Length")
            continue;

        // Transfer-Encoding no se reenvía: el servidor ya ha descodificado el body
        // chunked antes de pasarlo al CGI. Si enviáramos HTTP_TRANSFER_ENCODING=chunked,
        // el runtime CGI de Go (net/http/cgi) envolvería stdin en un chunked reader
        // esperando datos en formato chunked, pero recibe bytes planos → error de CGI.
        if (name == "Transfer-Encoding")
            continue;

        // Construir el nombre de la variable: "HTTP_" + nombre en mayúsculas con '-' → '_'
        std::string envname = "HTTP_";
        for (size_t k = 0; k < name.size(); k++) {
            char c = name[k];
            envname += (c == '-') ? '_' : static_cast<char>(std::toupper(c));
        }

        env.push_back(envname + "=" + value);
    }

    return env;
}


// ---------- Constructor por defecto (solo uso interno) ----------

Response::Response() :
    _statusCode("200"), _body(""), _contentType("text/html"),
    _extraHeaders(""), _isHead(false), _headContentLength(0) {}

// ---------- startCGIAsync: fork + pipes sin bloquear ----------
//
// Detecta si el request necesita CGI, crea los pipes, hace fork y
// lanza execve en el hijo. El padre retorna inmediatamente con CgiState.
// El event loop principal gestiona stdin/stdout del CGI desde aquí.
//
// Retorna un CgiState con pid == -1 si el request NO es CGI.

CgiState Response::startCGIAsync(const Request& req, Server& srv,
                                   size_t body_remaining, size_t full_content_length) {
    CgiState state;
    state.pid            = -1;
    state.stdin_fd       = -1;
    state.stdout_fd      = -1;
    state.input_written  = 0;
    state.input_done     = false;
    state.headers_sent   = false;
    state.start_time     = time(NULL);
    state.server_idx     = 0;
    state.body_remaining = body_remaining;

    std::string method = req.getMethod();
    if (method != "GET" && method != "POST")
        return state; // solo GET y POST van a CGI

    std::string target = req.getPath().empty() ? req.getTarget() : req.getPath();
    const std::vector<Location> locs = srv.getLocation();

    // Buscar intérprete CGI por extensión del target (misma lógica que el constructor)
    std::string interpreter;
    std::string scriptPath;
    {
        size_t dotPos = target.find_last_of('.');
        if (dotPos != std::string::npos) {
            std::string fileExt = target.substr(dotPos);
            for (size_t i = 0; i < locs.size(); i++) {
                if (!locs[i].getCgiExt().empty() &&
                    locs[i].getCgiExt() == fileExt &&
                    !locs[i].getCgiPath().empty()) {
                    interpreter = locs[i].getCgiPath();
                    break;
                }
            }
        }
    }
    if (interpreter.empty())
        return state; // no es CGI

    // Calcular la ruta física del script (misma lógica que el constructor)
    int locIdx = findLocation(locs, target);
    std::string root = srv.getRoot();
    if (locIdx > -1)
        root = locs[locIdx].getRoot();

    std::string effectiveTarget = target;
    if (locIdx > -1) {
        std::string locPath = locs[locIdx].getLocation();
        if (target.size() >= locPath.size() && target.substr(0, locPath.size()) == locPath) {
            effectiveTarget = target.substr(locPath.size());
            if (effectiveTarget.empty())
                effectiveTarget = "/";
            else if (effectiveTarget[0] != '/')
                effectiveTarget = "/" + effectiveTarget;
        }
    }
    scriptPath = replaceDoubleSlashes(root + effectiveTarget);

    // Extraer directorio y nombre del script para chdir() en el hijo
    std::string scriptDir  = scriptPath.substr(0, scriptPath.rfind('/') + 1);
    std::string scriptName = scriptPath.substr(scriptPath.rfind('/') + 1);

    // Si el intérprete es relativo (ej: ./cgi_tester), después del chdir()
    // ya no se encontraría. Calculamos la ruta relativa al nuevo cwd
    // contando cuántos niveles tiene scriptDir y añadiendo "../" por cada uno.
    std::string resolvedInterpreter = interpreter;
    if (!interpreter.empty() && interpreter[0] != '/') {
        std::string relInterp = interpreter;
        if (relInterp.size() >= 2 && relInterp[0] == '.' && relInterp[1] == '/')
            relInterp = relInterp.substr(2);
        std::string dir = scriptDir;
        if (dir.size() >= 2 && dir[0] == '.' && dir[1] == '/')
            dir = dir.substr(2);
        std::string prefix;
        for (size_t k = 0; k < dir.size(); k++) {
            if (dir[k] == '/')
                prefix += "../";
        }
        resolvedInterpreter = prefix + relInterp;
    }

    // Preparar argv (misma lógica que startCGIscript)
    std::string body = req.getBody();
    char* argv_bash[] = {
        const_cast<char*>(resolvedInterpreter.c_str()),
        const_cast<char*>(scriptName.c_str()),
        const_cast<char*>(body.c_str()),
        NULL
    };
    char* argv_cgi[] = {
        const_cast<char*>(resolvedInterpreter.c_str()),
        const_cast<char*>(scriptName.c_str()),
        NULL
    };
    char** argv = (interpreter == "/bin/bash") ? argv_bash : argv_cgi;

    int stdin_pipe[2];
    int stdout_pipe[2];
    if (pipe(stdin_pipe) == -1 || pipe(stdout_pipe) == -1) {
        perror("CGI pipe error");
        return state;
    }

    pid_t pid = fork();
    if (pid == -1) {
        perror("CGI fork error");
        close(stdin_pipe[0]);  close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        return state;
    }

    if (pid == 0) {
        // Hijo: redirigir stdin/stdout y ejecutar el CGI
        dup2(stdin_pipe[0],  STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        // Cerrar TODOS los fds heredados del padre (> stderr).
        // Sin esto, el hijo mantiene abiertos los write-ends de los pipes
        // de OTROS CGIs concurrentes, impidiendo el EOF en esos pipes
        // y causando deadlock cuando hay múltiples CGIs simultáneos.
        for (int fd_i = 3; fd_i < 1024; fd_i++)
            close(fd_i);
        //el proceso hijo hereda el PWD del servidor (por ejemplo /home/gustgonz/cursus_42/Webserv/)
        if (chdir(scriptDir.c_str()) != 0)
            perror("CGI chdir error");
        std::vector<std::string> envStrs = buildCGIEnvVec(req, srv, body, full_content_length);
        std::vector<char*> envp;
        for (size_t ei = 0; ei < envStrs.size(); ei++)
            envp.push_back(const_cast<char*>(envStrs[ei].c_str()));
        envp.push_back(NULL);
        execve(resolvedInterpreter.c_str(), argv, &envp[0]);
        perror("CGI execve error");
        exit(1);
    }

    // Padre: cerrar los extremos del hijo, poner los nuestros en O_NONBLOCK
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    fcntl(stdin_pipe[1],  F_SETFL, O_NONBLOCK);
    fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(stdin_pipe[1],  F_SETPIPE_SZ, 1048576);
    fcntl(stdout_pipe[0], F_SETPIPE_SZ, 1048576);

    state.pid        = pid;
    state.stdout_fd  = stdout_pipe[0];
    state.input_buf  = body;
    // Cerrar stdin solo si no hay body ahora NI se esperan más bytes de TCP.
    // Con early CGI start, body puede ser vacío pero body_remaining > 0.
    if (body.empty() && body_remaining == 0) {
        close(stdin_pipe[1]);
        state.stdin_fd   = -1;
        state.input_done = true;
    } else {
        state.stdin_fd   = stdin_pipe[1];
        state.input_done = false;
    }
    return state;
}

// ---------- buildFromCGIOutput: parsea salida CGI → HTTP response ----------

std::string Response::buildFromCGIOutput(const std::string& cgiOut,
                                          const Request& req, Server& srv) {
    (void)req;
    Response r;

    if (cgiOut.empty()) {
        r._statusCode = "500";
        r._body       = readFromFile(srv.getRoot() + srv.getErrorPage(500));
        return r.build();
    }

    size_t sepPos = cgiOut.find("\r\n\r\n");
    if (sepPos == std::string::npos) {
        r._body       = cgiOut;
        r._statusCode = "200";
        return r.build();
    }

    std::string cgiHeaders = cgiOut.substr(0, sepPos);
    r._body       = cgiOut.substr(sepPos + 4);
    r._statusCode = "200";

    std::istringstream hss(cgiHeaders);
    std::string hline;
    while (std::getline(hss, hline)) {
        if (!hline.empty() && hline[hline.size() - 1] == '\r')
            hline.erase(hline.size() - 1);
        if (hline.find("Status:") == 0) {
            std::string val = hline.substr(7);
            size_t s = val.find_first_not_of(" ");
            if (s != std::string::npos) {
                size_t e = val.find(' ', s);
                r._statusCode = (e != std::string::npos) ? val.substr(s, e - s) : val.substr(s);
            }
        } else if (!hline.empty() && hline.find("Content-Type:") == 0) {
            std::string val = hline.substr(13);
            size_t s = val.find_first_not_of(" ");
            if (s != std::string::npos)
                r._contentType = val.substr(s);
        } else if (!hline.empty()) {
            r._extraHeaders += hline + "\r\n";
        }
    }
    return r.build();
}

// ---------- Constructor: inicializa y despacha la respuesta ----------
//
// Flujo principal:
//   1. Inicializar campos de la respuesta.
//   2. Buscar la location que mejor coincide con el target.
//   3. Si la location tiene redirección 301 → responder y salir.
//   4. Construir la ruta física del recurso.
//   5. Delegar a handleDirectory, handleGET, handlePOST o handleDELETE.

Response::Response(const Request& req, Server& srv) {
    // Inicializar los campos con valores por defecto
    _statusCode       = "";
    _body             = "";
    _contentType      = "text/html";
    _extraHeaders     = "";
    _isHead           = false;
    _headContentLength = 0;

    std::string method = req.getMethod();
    std::string target = req.getTarget();
    std::string body   = req.getBody();

    // Guardar el vector una sola vez para evitar copias repetidas por cada llamada a getLocation()
    const std::vector<Location> locs = srv.getLocation();

    // Buscar la location cuyo prefijo coincide mejor con el target pedido
    int         locIdx         = findLocation(locs, target);
    std::string root           = srv.getRoot();
    std::string allowedMethods = "";

    // Si se encontró una location, aplicar su configuración
    if (locIdx > -1) {
        root           = locs[locIdx].getRoot();
        allowedMethods = locs[locIdx].getMethods();

        // Si la location define una redirección 301, responder inmediatamente sin procesar más
        std::string redirect = locs[locIdx].getRedirectPage(301);
        if (!redirect.empty()) {
            _body       = "<!DOCTYPE html>\n<a href=" + redirect + ">Click here</a>";
            _statusCode = "301";
            return;
        }
    }

    // HEAD: si "HEAD" no está explícitamente en allow_methods → 405.
    // El tester espera métodos estrictos: allow_methods GET no implica HEAD.       ACA HAY QUE VERIFICAR
    if (method == "HEAD" && !allowedMethods.empty() && allowedMethods.find("HEAD") == std::string::npos) {
        _statusCode = "405";
        _body = readFromFile(srv.getRoot() + srv.getErrorPage(405));
        _isHead = true;
        _headContentLength = _body.size();
        _body = "";
        return;
    }

    // Calcular el "effective target": se quita el prefijo de la location del URL.
    //
    // ¿Por qué? En nginx, cuando una location tiene su propio "root", ese root
    // REEMPLAZA el prefijo del URL — no se concatena con él.
    //
    // Ejemplo con: location /directory { root ./YoupiBanane; }
    //   target          = /directory/youpi.bla
    //   locPath         = /directory
    //   effectiveTarget = /youpi.bla           (se quitó "/directory")
    //   path            = ./YoupiBanane/youpi.bla  ← archivo real en disco
    //
    // Sin esto:
    //   path = root + target = ./YoupiBanane/directory/youpi.bla  ← no existe → 404
    std::string effectiveTarget = target;
    if (locIdx > -1) {
        std::string locPath = locs[locIdx].getLocation();
        // Para GET/HEAD: strip aunque el target coincida exactamente con el locPath,
        // para que /directory resuelva a ./YoupiBanane/ (la raíz de la location).
        // Para POST/DELETE: solo strip si target es MAYOR, para que /Post resuelva
        // a ./www/db/Post y pueda crear Post.txt (no .txt en la raíz).
        bool exactMatchOk = (method == "GET" || method == "HEAD");
        bool sizeOk = exactMatchOk ? (target.size() >= locPath.size())
                                   : (target.size() > locPath.size());
        if (sizeOk && target.substr(0, locPath.size()) == locPath) {
            effectiveTarget = target.substr(locPath.size());
            if (effectiveTarget.empty())
                effectiveTarget = "/"; // target era exactamente el prefijo → raíz del location
            else if (effectiveTarget[0] != '/')
                effectiveTarget = "/" + effectiveTarget;
        }
    }

    // Construir la ruta física eliminando dobles barras
    std::string path = replaceDoubleSlashes(root + effectiveTarget);
    // Buscar si el target tiene una extensión CGI configurada en CUALQUIER location
    // del servidor (no solo en la location que hizo match).
    // Regla del tester: "Any file with .bla extension must answer to POST by calling
    // the cgi_test executable" — la extensión es una regla global, no por location.
    std::string foundCgiInterpreter;               // ES NECESARIO TRAER TODO EL VECTOR LOC DEL SERVER?
    {
        size_t dotPos = target.find_last_of('.');
        if (dotPos != std::string::npos) {
            std::string fileExt = target.substr(dotPos); // e.g., ".bla"
            for (size_t i = 0; i < locs.size(); i++) {
                if (!locs[i].getCgiExt().empty() &&
                    locs[i].getCgiExt() == fileExt &&
                    !locs[i].getCgiPath().empty()) {
                    foundCgiInterpreter = locs[i].getCgiPath();
                    break;
                }
            }
        }
    }
    if (!foundCgiInterpreter.empty() && (method == "POST" || method == "GET")) {
        // CGI interceptado por startCGIAsync() en Server.cpp antes de llegar aquí.
        // Este bloque no debería alcanzarse en ejecución normal.
        _statusCode = "500";
        _body       = readFromFile(srv.getRoot() + srv.getErrorPage(500));
        return;
    }

    // Determinar si el recurso es un directorio o delegar según el método HTTP.
    // Solo se entra en el bloque de directorio para GET: POST y DELETE deben
    // llegar a sus manejadores aunque el path resuelva a un directorio existente
    // (ej: POST a /db que resuelve a ./www_server2/db/).
    struct stat fileStat;
    if (!stat(path.c_str(), &fileStat) && S_ISDIR(fileStat.st_mode) && (method == "GET" || method == "HEAD"))
    {
        if (!allowedMethods.empty() && allowedMethods.find("GET") == std::string::npos) {
            _statusCode = "405";
            _body += readFromFile(srv.getRoot() + srv.getErrorPage(405));
        } else {
            handleDirectory(path, locIdx, req, srv);
        }
    }
    // El path no es un directorio existente (o el método no es GET/HEAD): tratar como archivo
    else if (method == "GET" || method == "HEAD")
        handleGET(path, effectiveTarget, allowedMethods, req, srv);
    else if (method == "POST") {
        handlePOST(path, body, allowedMethods, srv, req);
    }
    else if (method == "DELETE")
        handleDELETE(path, allowedMethods, srv);
    else if (method == "OPTIONS") {
        // Problema raíz: el browser envía un preflight OPTIONS antes de ejecutar
        // requests "peligrosas" (DELETE, POST multipart) hacia un origen distinto.
        // Esto ocurre cuando el JS usa "http://localhost:8001" pero el browser
        // accede desde "http://127.0.0.1:8001" — para el browser son orígenes
        // diferentes aunque apunten al mismo servidor.
        // Si el servidor responde 501, el browser bloquea la request real y nunca
        // llegan el DELETE ni el upload.
        // Solución: responder 204 No Content al OPTIONS. El browser interpreta esto
        // como "el servidor acepta la operación" y procede con la request real.
        _statusCode = "204";
        _body = "";
    }
    else {
        // Método HTTP no soportado por el servidor
        _statusCode = "501";
        _body += readFromFile(srv.getRoot() + srv.getErrorPage(501));
    }
    // HEAD: mismos headers que GET pero sin body (RFC 7231 §4.3.2).
    // Problema raíz: si borramos _body antes de build(), Content-Length queda a 0,
    // pero el RFC exige que sea igual al tamaño real del recurso (igual que GET).
    // El tester compara Content-Length de HEAD con el de GET y falla si difieren.
    // Solución: guardar el tamaño real en _headContentLength ANTES de borrar _body,
    // y usarlo en build() en lugar de _body.length().
    if (method == "HEAD") {
        _isHead = true;
        _headContentLength = _body.size();
        _body = "";
    }

}

// ---------- Manejador de directorios ----------
//
// Flujo:
//   1. Intentar servir el archivo index (de la location o del servidor).
//   2. Si no hay index y autoindex está activo → generar listado via CGI.
//   3. Si no hay index ni autoindex → 403 Forbidden.

void Response::handleDirectory(const std::string& path, int locIdx, const Request& req, const Server& srv) {
    // Obtener el archivo index: primero el de la location, luego el del servidor
    std::string indexFile = (locIdx > -1) ? srv.getLocation()[locIdx].getIndex() : "";
    if (indexFile.empty())
        indexFile = srv.getIndex();

    // Si existe un archivo index configurado, intentar abrirlo y servirlo.
    // Si el index está configurado pero NO existe en este directorio → 404.
    //
    // ¿Por qué 404 y no autoindex? Porque el tester espera que directorios sin
    // el index configurado (ej: Yeah/ que no tiene youpi.bad_extension) devuelvan
    // 404: el recurso específico buscado no existe, aunque el directorio sí exista.
    // Si no hay index configurado en absoluto → ahí sí se cae al autoindex/403.
    if (!indexFile.empty()) {
        std::string indexPath = replaceDoubleSlashes(path + "/" + indexFile);
        std::ifstream idxF(indexPath.c_str());
        if (idxF.is_open()) {
            // Index encontrado → servir el archivo
            std::string content;
            char ch;
            while (idxF.get(ch))
                content += ch;
            idxF.close();
            _body       = content;
            _statusCode = "200";
            return;
        }
        // Index configurado pero no encontrado en este directorio → 404
        _statusCode = "404";
        _body += readFromFile(srv.getRoot() + srv.getErrorPage(404));
        return;
    }

    // Sin index: verificar si el autoindex está habilitado en la configuración
    bool autoindex = (locIdx > -1) ? srv.getLocation()[locIdx].getAutoindex() : false;
    if (autoindex) {
        std::string urlPath = req.getTarget();
        if (!urlPath.empty() && urlPath[urlPath.size() - 1] == '/')
            urlPath.erase(urlPath.size() - 1);
        DIR *dir = opendir(path.c_str());
        if (dir) {
            _body  = "<!DOCTYPE html>\n<html>\n<head><meta charset=\"UTF-8\">";
            _body += "<title>Index of " + urlPath + "</title></head>\n";
            _body += "<body>\n<h1>Index of " + urlPath + "</h1>\n<hr>\n<ul>\n";
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                std::string name = entry->d_name;
                if (name == ".")
                    continue;
                if (entry->d_type == DT_DIR)
                    _body += "<li><a href=\"" + urlPath + "/" + name + "/\">" + name + "/</a></li>\n";
                else
                    _body += "<li><a href=\"" + urlPath + "/" + name + "\">" + name + "</a></li>\n";
            }
            closedir(dir);
            _body += "</ul>\n<hr>\n</body>\n</html>\n";
            _statusCode = "200";
        } else {
            _statusCode = "500";
            _body += readFromFile(srv.getRoot() + srv.getErrorPage(500));
        }
    } else {
        // Sin index ni autoindex: acceso prohibido
        _statusCode = "403";
        _body += readFromFile(srv.getRoot() + srv.getErrorPage(403));
    }
}

// ---------- Manejador GET ----------
//
// Flujo:
//   1. Verificar que GET esté permitido en la location → 405 si no.
//   2. Si el target tiene extensión CGI → ejecutar script.
//   3. Si no → leer y servir el archivo estático → 404 si no existe.

void Response::handleGET(const std::string& path, const std::string& target,
                         const std::string& allowedMethods, const Request& req, const Server& srv) {
    (void)req;
    // Verificar que el método GET esté permitido en esta location
    if (allowedMethods.find("GET") == std::string::npos) {
        _statusCode = "405";
        _body += readFromFile(srv.getRoot() + srv.getErrorPage(405));
        return;
    }

    // Archivo estático: si no tiene extensión, buscar como .txt
    std::string filePath = (target.find('.') == std::string::npos) ? path + ".txt" : path;
    std::ifstream file(filePath.c_str());
    if (!file.is_open()) {
        std::cerr << "Error opening file" << std::endl;
        _statusCode = "404";
        _body += readFromFile(srv.getRoot() + srv.getErrorPage(404));
        return;
    }

    // Detectar MIME type según extensión del archivo
    size_t dotPos = filePath.rfind('.');
    if (dotPos != std::string::npos) {
        std::string ext = filePath.substr(dotPos);
        if      (ext == ".html" || ext == ".htm") _contentType = "text/html";
        else if (ext == ".css")                   _contentType = "text/css";
        else if (ext == ".js")                    _contentType = "application/javascript";
        else if (ext == ".json")                  _contentType = "application/json";
        else if (ext == ".pdf")                   _contentType = "application/pdf";
        else if (ext == ".png")                   _contentType = "image/png";
        else if (ext == ".jpg" || ext == ".jpeg") _contentType = "image/jpeg";
        else if (ext == ".gif")                   _contentType = "image/gif";
        else if (ext == ".svg")                   _contentType = "image/svg+xml";
        else if (ext == ".ico")                   _contentType = "image/x-icon";
        else if (ext == ".txt")                   _contentType = "text/plain";
        else                                      _contentType = "application/octet-stream";
    }

    // Leer el contenido del archivo y devolver 200 OK
    std::string content;
    char ch;
    while (file.get(ch))
        content += ch;
    file.close();
    _body       = content;
    _statusCode = "200";
}

// ---------- Helper: extraer el filename del header Content-Disposition ----------
// Ejemplo: "Content-Disposition: form-data; name="file"; filename="foto.jpg""
// Retorna "foto.jpg" o cadena vacía si no hay filename
static std::string extractFilename(const std::string& headers)
{
    size_t pos = headers.find("filename=\"");
    if (pos == std::string::npos)
        return "";
    pos += 10; // saltar los 10 caracteres de 'filename="'
    size_t end = headers.find("\"", pos);
    if (end == std::string::npos)
        return "";
    return headers.substr(pos, end - pos);
}

// ---------- Manejador de subida de archivos multipart ----------
//
// El body multipart tiene esta estructura:
//   --boundary\r\n
//   Content-Disposition: form-data; name="file"; filename="foto.jpg"\r\n
//   Content-Type: image/jpeg\r\n
//   \r\n
//   <bytes del archivo>
//   \r\n--boundary--\r\n
//
// Esta función extrae cada parte y guarda el archivo en disco.

void Response::handleMultipart(const std::string& path, const std::string& body,
                                const std::string& boundary)
{
    // El delimitador de cada parte es "--" + boundary
    std::string delim = "--" + boundary;
    size_t pos = 0;
    std::string lastSaved = "";

    while (true)
    {
        // Buscar el inicio del siguiente delimitador en el body
        size_t delimStart = body.find(delim, pos);
        if (delimStart == std::string::npos)
            break;

        // Avanzar más allá del texto del delimitador
        size_t afterDelim = delimStart + delim.size();

        // Si los dos caracteres siguientes son "--" es el delimitador final → terminar
        if (afterDelim + 2 <= body.size() && body.substr(afterDelim, 2) == "--")
            break;

        // Saltar el \r\n obligatorio que sigue al delimitador
        if (afterDelim + 2 <= body.size() && body.substr(afterDelim, 2) == "\r\n")
            afterDelim += 2;

        // Buscar el fin de los headers de esta parte (están separados del contenido por \r\n\r\n)
        size_t headerEnd = body.find("\r\n\r\n", afterDelim);
        if (headerEnd == std::string::npos)
            break;

        // Extraer los headers de esta parte (Content-Disposition, Content-Type, etc.)
        std::string partHeaders = body.substr(afterDelim, headerEnd - afterDelim);

        // Obtener el nombre real del archivo desde el header Content-Disposition
        std::string filename = extractFilename(partHeaders);

        // El contenido del archivo empieza justo después del doble CRLF
        size_t contentStart = headerEnd + 4;

        // El contenido termina justo antes del \r\n que precede al siguiente delimitador
        size_t contentEnd = body.find("\r\n" + delim, contentStart);
        if (contentEnd == std::string::npos)
            break;

        // Extraer los bytes exactos del archivo (puede ser binario: foto, PDF, etc.)
        std::string fileContent = body.substr(contentStart, contentEnd - contentStart);

        // Construir la ruta de destino:
        //   Si path es un directorio existente → guardar dentro de él (ej: ./www/db/Post/foto.jpg)
        //   Si path NO es un directorio (ej: ./www/db/upload/upload) → usar el directorio padre
        //   Sin filename → path.txt (comportamiento original para texto plano)
        std::string saveDir;
        struct stat dirCheck;
        if (stat(path.c_str(), &dirCheck) == 0 && S_ISDIR(dirCheck.st_mode))
            saveDir = path; // path ya es un directorio válido
        else {
            // path no existe como directorio → subir un nivel para obtener el directorio real
            size_t lastSlash = path.find_last_of('/');
            saveDir = (lastSlash != std::string::npos) ? path.substr(0, lastSlash) : ".";
        }

        std::string savePath;
        if (!filename.empty())
            savePath = saveDir + "/" + filename;
        else
            savePath = path + ".txt";

        std::cout << "Guardando archivo en: " << savePath << std::endl;

        // Abrir en modo binario para preservar todos los bytes sin modificarlos
        std::ofstream outFile(savePath.c_str(), std::ios::out | std::ios::binary);
        if (!outFile.is_open())
        {
            std::cerr << "Error al crear el archivo: " << savePath << std::endl;
            _statusCode = "500";
            return;
        }
        outFile.write(fileContent.c_str(), fileContent.size());
        outFile.close();
        lastSaved = savePath;

        // Avanzar para buscar la siguiente parte del multipart
        pos = contentEnd;
    }

    _statusCode = "201";
    _body = "<h1>Exito!</h1><p>Archivo subido: " + lastSaved + "</p>";
}

// ---------- Manejador POST ----------
//
// Flujo:
//   1. Verificar que POST esté permitido en la location → 405 si no.
//   2. Si Content-Type es multipart/form-data → extraer y guardar el archivo real.
//   3. Si es texto plano → guardar el body como .txt → 201 Created.

void Response::handlePOST(const std::string& path, const std::string& body,
                          const std::string& allowedMethods, const Server& srv,
                          const Request& req) {
    // Verificar que el método POST esté permitido en esta location
    if (allowedMethods.find("POST") == std::string::npos) {
        _statusCode = "405";
        _body += readFromFile(srv.getRoot() + srv.getErrorPage(405));
        return;
    }

    // Si el cliente envió un archivo con multipart/form-data, parsearlo correctamente
    // para extraer el archivo real en lugar de guardar el body crudo con los headers del multipart
    if (req.getContentType() == "multipart/form-data" && !req.getBoundary().empty())
    {
        handleMultipart(path, body, req.getBoundary());
        return;
    }

    // Caso texto plano: guardar el body directamente como archivo .txt
    std::string filePath = path + ".txt";
    std::cout << filePath << std::endl;
    std::ofstream file(filePath.c_str(), std::ios::out);
    if (!file.is_open()) {
        std::cerr << "Error opening file: " << std::endl;
        std::cerr << "Unable to open the file." << std::endl;
        _statusCode = "500";
        _body += readFromFile(srv.getRoot() + srv.getErrorPage(500));
        return;
    }

    // Escribir el cuerpo y confirmar que el recurso fue creado
    file << body << std::endl;
    file.close();
    _statusCode = "201";
    _body  = "<h1>Exito!</h1><p>Archivo creado: " + filePath + "</p>";
    _body += "<pre>" + body + "</pre>";
    std::cout << "The file has been written correctly." << std::endl;
}

// ---------- Manejador DELETE ----------
//
// Flujo:
//   1. Verificar que DELETE esté permitido en la location → 405 si no.
//   2. Eliminar el archivo .txt correspondiente → 500 si falla, 200 si OK.
// "Checking errno after read/write/recv/send is strictly forbidden"
// Estas son operaciones de red (sockets). remove() es una operación de disco (filesystem) 
// — no es read/write/recv/send sobre un socket.

void Response::handleDELETE(const std::string& path, const std::string& allowedMethods,
                            const Server& srv) {
    // Verificar que el método DELETE esté permitido en esta location
    if (allowedMethods.find("DELETE") == std::string::npos) {
        _statusCode = "405";
        _body += readFromFile(srv.getRoot() + srv.getErrorPage(405));
        return;
    }
    std::string filePath = path;
    int rm_result = remove(filePath.c_str());
    int rm_errno  = errno;
    if (rm_result != 0) {
        if (rm_errno == ENOENT) {
            _statusCode = "404";
            _body += readFromFile(srv.getRoot() + srv.getErrorPage(404));
        } else {
            _statusCode = "500";
            _body += readFromFile(srv.getRoot() + srv.getErrorPage(500));
        }
    } else {
        _statusCode = "200";
    }
}

Response::~Response() {}

// ---------- Build the HTTP response string ----------

static std::string getReasonPhrase(const std::string& code) {
    if (code == "200") return "OK";
    if (code == "201") return "Created";
    if (code == "204") return "No Content";
    if (code == "301") return "Moved Permanently";
    if (code == "400") return "Bad Request";
    if (code == "403") return "Forbidden";
    if (code == "404") return "Not Found";
    if (code == "405") return "Method Not Allowed";
    if (code == "413") return "Content Too Large";
    if (code == "500") return "Internal Server Error";
    if (code == "501") return "Not Implemented";
    return "Unknown";
}

std::string Response::build() const {
    std::ostringstream oss;
    // Para HEAD: usar el tamaño real del recurso, no el body vacío
    oss << (_isHead ? _headContentLength : _body.length());

    std::string response;
    response += "HTTP/1.1 ";
    response += _statusCode;
    response += " ";
    response += getReasonPhrase(_statusCode);
    response += "\r\n";
    response += "Content-Length: ";
    response += oss.str();
    response += "\r\n";
    response += "Content-Type: ";
    response += _contentType;
    response += "\r\n";
    // Cabeceras CGI adicionales reenvíadas desde la salida del script CGI
    response += _extraHeaders;
    // Problema raíz: sin estos headers el browser bloquea las respuestas del
    // servidor cuando el origen de la página difiere del host de la request
    // (ej: 127.0.0.1 vs localhost). El browser exige que el servidor declare
    // explícitamente qué orígenes, métodos y headers acepta.
    // Solución: incluir los headers CORS en todas las respuestas para que el
    // browser permita que el JS lea la respuesta y ejecute las requests reales.
    response += "Access-Control-Allow-Origin: *\r\n";
    response += "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n";
    response += "Access-Control-Allow-Headers: Content-Type\r\n";
    response += "\r\n";
    response += _body;
    return response;
}
//No se usan en ningún lado del código. Son getters que se definieron pero nunca se llamaron 
std::string Response::getStatusCode() const { return _statusCode; }
std::string Response::getBody() const { return _body; }
