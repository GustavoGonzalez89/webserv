// =============================================================================
// Server.cpp — Núcleo del servidor HTTP
// =============================================================================
//
// Este archivo tiene dos responsabilidades claramente separadas:
//
//  [PARTE A — Líneas 1–279] Construcción del servidor desde el .conf
//  ─────────────────────────────────────────────────────────────────
//  El ConfigParser entrega a Server un string crudo con el contenido del bloque
//  "location { ... }". Las funciones estáticas de esta sección parsean ese string
//  y extraen cada directiva (root, methods, cgi_ext, etc.) para construir objetos
//  Location. Al final del constructor, el servidor ya sabe qué rutas existen,
//  qué métodos permiten y si tienen CGI.
//
//  [PARTE B — Líneas 380–964] Bucle de eventos (Server::run)
//  ──────────────────────────────────────────────────────────
//  Es el corazón del servidor. Un único select() vigila todos los fds a la vez:
//  sockets de escucha, conexiones de clientes y pipes de procesos CGI.
//  Por cada ciclo del bucle:
//    1. select() bloquea hasta que algún fd tiene actividad
//    2. Se gestionan los CGIs en vuelo (escribir body al hijo, leer su respuesta)
//    3. Se aceptan nuevas conexiones entrantes
//    4. Se leen bytes de clientes → se acumulan → se detecta request completo →
//       se construye la respuesta
//    5. Se envían las respuestas pendientes
//
// =============================================================================

#include "Server.hpp"
#include "Request.hpp"
#include "Response.hpp"

#include <signal.h>
extern volatile sig_atomic_t g_running;

// =============================================================================
// PARTE A — Helpers de parseo del bloque location
// =============================================================================
//
// El ConfigParser no parsea los bloques location: los entrega como strings crudos
// a Server::loadLocationBlocks(). Estas funciones estáticas son las herramientas
// de extracción: cada una busca su keyword en ese string, recorta hasta el ";" y
// devuelve el valor limpio. Son internas a este archivo (static), nadie más las usa.
// =============================================================================

// trimStr — Elimina espacios, tabs y saltos de línea al inicio y al final de un string.
// Se usa después de cada extracción porque el valor entre la keyword y el ";" puede
// tener espacios arbitrarios (ej: "root   ./www  ;").
static std::string trimStr(const std::string& str) {
    size_t head = str.find_first_not_of(" \t\n\r\f\v");
    size_t tail = str.find_last_not_of(" \t\n\r\f\v");
    if (head == std::string::npos || tail == std::string::npos)
        return "";
    return str.substr(head, tail - head + 1);
}

// extractPath — Extrae el path de la declaración "location /ruta { ... }".
// El bloque que recibe ya tiene la keyword "location" eliminada, así que el
// primer token no-espacio que encuentra es directamente el path ("/upload", "/cgi-bin", etc.).
// Ese path se usará después para hacer longest-prefix matching cuando llegue un request.
static std::string extractPath(std::string input) {
    std::string result;
    for (size_t i = 0; i < input.length(); ++i)
    {
        if (std::isspace(input[i]))
            continue;
        while (i < input.length() && !std::isspace(input[i]) && input[i] != ';')
        {
            result += input[i];
            ++i;
        }
        break;
    }
    return result;
}

// extractLocRoot — Extrae el filesystem root de la directiva "root ./www/uploads;".
// Si una location tiene su propio root, los archivos se sirven desde esa ruta en
// lugar del root global del servidor. Si no existe la directiva, devuelve "".
static std::string extractLocRoot(std::string input) {
    size_t kw_pos  = input.find("root");
    size_t sc_pos  = input.find(";", kw_pos);
    std::string val;
    if (kw_pos != std::string::npos && sc_pos != std::string::npos) {
        size_t start  = kw_pos + std::strlen("root");
        size_t length = sc_pos - start;
        val = input.substr(start, length);
    }
    return trimStr(val);
}

// extractLocIndex — Extrae el archivo index de la directiva "index index.html;".
// Cuando el cliente pide un directorio (ej: GET /), el servidor sirve este archivo
// por defecto en lugar de mostrar un listado o devolver 403.
static std::string extractLocIndex(std::string input) {
    size_t kw_pos  = input.find("\nindex");
    size_t sc_pos  = input.find(";", kw_pos);
    std::string val;
    if (kw_pos != std::string::npos && sc_pos != std::string::npos) {
        size_t start  = kw_pos + std::strlen(" index");
        size_t length = sc_pos - start;
        val = input.substr(start, length);
    }
    return trimStr(val);
}

// extractReturnVal — Extrae el valor de "return 301 /nueva-ruta;".
// Si la location tiene esta directiva, el servidor responde con un redirect
// sin llegar a buscar ningún archivo. Ej: "location /old { return 301 /new; }".
static std::string extractReturnVal(std::string input) {
    size_t kw_pos  = input.find("return");
    size_t sc_pos  = input.find(";", kw_pos);
    std::string val;
    if (kw_pos != std::string::npos && sc_pos != std::string::npos) {
        size_t start  = kw_pos + std::strlen("return");
        size_t length = sc_pos - start;
        val = input.substr(start, length);
    }
    return trimStr(val);
}

// extractAllowMethods — Extrae los métodos HTTP permitidos: "allow_methods GET POST;".
// Si el cliente usa un método no listado aquí, Response devuelve 405 Method Not Allowed.
// Si la directiva no existe, se asume que no hay restricción de métodos.
static std::string extractAllowMethods(std::string input) {
    size_t kw_pos  = input.find("allow_methods");
    size_t sc_pos  = input.find(";", kw_pos);
    std::string val;
    if (kw_pos != std::string::npos && sc_pos != std::string::npos) {
        size_t start  = kw_pos + std::strlen("allow_methods");
        size_t length = sc_pos - start;
        val = input.substr(start, length);
    }
    return trimStr(val);
}

// extractTryFiles — Extrae la directiva "try_files $uri /fallback;".
// Indica al servidor que pruebe varias rutas antes de responder 404.
static std::string extractTryFiles(std::string input) {
    size_t kw_pos  = input.find("try_files");
    size_t sc_pos  = input.find(";", kw_pos);
    std::string val;
    if (kw_pos != std::string::npos && sc_pos != std::string::npos) {
        size_t start  = kw_pos + std::strlen("try_files");
        size_t length = sc_pos - start;
        val = input.substr(start, length);
    }
    return trimStr(val);
}

// extractCgiPath — Extrae el ejecutable CGI: "cgi_path ./cgi_tester;".
// Es la ruta del intérprete que se pasará a execve() cuando llegue un request
// cuya extensión coincida con cgi_ext. Ej: "./cgi_tester", "/usr/bin/php-cgi".
static std::string extractCgiPath(std::string input) {
    size_t kw_pos  = input.find("cgi_path");
    size_t sc_pos  = input.find(";", kw_pos);
    std::string val;
    if (kw_pos != std::string::npos && sc_pos != std::string::npos) {
        size_t start  = kw_pos + std::strlen("cgi_path");
        size_t length = sc_pos - start;
        val = input.substr(start, length);
    }
    return trimStr(val);
}

// extractCgiExt — Extrae la extensión que activa el CGI: "cgi_ext .bla;".
// Cuando el target del request termina con esta extensión (ej: "/script.bla"),
// el servidor ejecuta el archivo como CGI en lugar de servirlo estáticamente.
static std::string extractCgiExt(std::string input) {
    size_t kw_pos  = input.find("cgi_ext");
    size_t sc_pos  = input.find(";", kw_pos);
    std::string val;
    if (kw_pos != std::string::npos && sc_pos != std::string::npos) {
        size_t start  = kw_pos + std::strlen("cgi_ext");
        size_t length = sc_pos - start;
        val = input.substr(start, length);
    }
    return trimStr(val);
}

// extractLocMaxBodySize — Extrae el límite de body para esta location: "client_max_body_size 100;".
// Devuelve 0 si la directiva no existe, lo que significa "usa el límite del servidor".
// Un valor > 0 sobreescribe el límite global solo para los requests que caigan en esta location.
// El event loop lo consulta antes de acumular el body completo para responder 413 a tiempo.
static int extractLocMaxBodySize(std::string input) {
    const std::string kw = "client_max_body_size";
    size_t kw_pos = input.find(kw);
    if (kw_pos == std::string::npos)
        return 0;
    size_t sc_pos = input.find(";", kw_pos);
    if (sc_pos == std::string::npos)
        return 0;
    size_t start  = kw_pos + kw.size();
    std::string val = trimStr(input.substr(start, sc_pos - start));
    return std::atoi(val.c_str());
}

// extractAutoindex — Devuelve true si "autoindex on;" está en el bloque.
// Cuando está activo y el cliente pide un directorio sin index, Response genera
// un HTML con el listado de archivos en lugar de devolver 403.
static bool extractAutoindex(std::string input) {
    size_t kw_pos  = input.find("autoindex");
    size_t sc_pos  = input.find(";", kw_pos);
    std::string val;
    if (kw_pos != std::string::npos && sc_pos != std::string::npos) {
        size_t start  = kw_pos + std::strlen("autoindex");
        size_t length = sc_pos - start;
        val = input.substr(start, length);
    }
    return (trimStr(val) == "on");
}

// extractRedirects — Acumula todos los valores de directivas "redirect" del bloque.
// Un location puede tener varias; se devuelven separadas por espacio para que
// Location las procese. Ej: "redirect 301 http://github.com;" → "301 http://github.com".
static std::string extractRedirects(std::string input) {
    std::string accumulated;
    size_t kw_pos = input.find("redirect");
    while (kw_pos != std::string::npos) {
        kw_pos += 8;
        size_t sc_pos = input.find(";", kw_pos);
        if (sc_pos != std::string::npos) {
            accumulated += trimStr(input.substr(kw_pos, sc_pos - kw_pos)) + " ";
            kw_pos = input.find("redirect", sc_pos);
        } else {
            break;
        }
    }
    return accumulated;
}

// =============================================================================
// Server — Métodos de construcción e inicialización
// =============================================================================

// operator<< — Imprime un resumen del servidor al arrancar.
// Sirve solo como confirmación visual de que el servidor cargó correctamente
// el puerto y el nombre. El bloque if(0) contiene el debug completo desactivado.
std::ostream &operator<<(std::ostream &stream, const Server &server) {
    if (0) {
        const std::map<int, std::string>& map = server.getErrorPageMap();
        stream << \
        "Server Name: " << server.getServerName() << std::endl << \
        "   Hostname: " << server.getHost()        << std::endl << \
        "       Port: " << server.getListen()      << std::endl << \
        "       Root: " << server.getRoot()        << std::endl << \
        "  MaxBodySz: " << server.getMaxBodySize() << std::endl << \
        "      Index: " << server.getIndex()       << std::endl << \
        " ServerSock: " << server.getSocket()      << std::endl;
        for (std::map<int, std::string>::const_iterator it = map.begin(); it != map.end(); ++it)
        {
            stream << \
        " Error Code: " << it->first << " handled by " << it->second << std::endl;
        }
        for (size_t i = 0; i < server.getLocation().size(); i++)
        {
            stream << server.getLocation()[i] << std::endl;
        }
        stream << "\033[38;5;51m================================================================\033[0m" << std::endl;
        stream << "Server name: " << server.getServerName() << ", listening on port " << server.getListen() << std::endl;
        stream << "domain: " << AF_INET << "| type: " << SOCK_STREAM << "| protocol:" << 0 << std::endl;
        stream << "\033[38;5;51m================================================================\033[0m" << std::endl;
    } else {
        stream << "\033[38;5;51m================================================================\033[0m" << std::endl;
        stream << "Server name: " << server.getServerName() << ", listening on port " << server.getListen() << std::endl;
        stream << "\033[38;5;51m================================================================\033[0m" << std::endl;
    }
    return (stream);
}

// loadErrorPages — Puebla el mapa _err_map con los pares (código → ruta) del .conf.
// El ConfigParser entrega una cadena como "404 /errors/404.html 500 /errors/500.html".
// Se parsea con istringstream leyendo pares int+string sucesivos.
// Response consulta _err_map cuando necesita construir una respuesta de error.
void Server::loadErrorPages(const std::string& error_patterns) {
    std::istringstream token_stream(error_patterns);
    int         err_code;
    std::string err_path;
    while (token_stream >> err_code >> err_path)
        this->_err_map[err_code] = err_path;
}

// extractLocBlock — Extrae el contenido del próximo bloque "location /ruta { ... }"
// del string de configuración, actualizando scan_pos para la siguiente llamada.
//
// El bloque puede estar anidado (location dentro de location), por eso se usa
// un contador de profundidad (depth): cada "{" suma 1, cada "}" resta 1.
// Cuando depth vuelve a 0 hemos cerrado el bloque correcto.
// loadLocationBlocks llama a esta función en bucle hasta que no quedan bloques.
static std::string extractLocBlock(const std::string& input, size_t& scan_pos, size_t& loc_found) {
    size_t kw_pos    = input.find("location", scan_pos);
    size_t open_br   = input.find("{", scan_pos);
    size_t close_br  = input.find('}', open_br);
    size_t depth     = 0;
    while (kw_pos != std::string::npos && open_br != std::string::npos && close_br != std::string::npos)
    {
        loc_found = (loc_found == 0) ? 1 : loc_found;
        size_t inner_open = input.find('{', open_br + 1);
        if (inner_open != std::string::npos && inner_open < close_br)
        {
            depth    = (depth == 0) ? 1 : depth;
            open_br  = inner_open;
            close_br = input.find('}', inner_open);
        } else {
            size_t next_open  = input.find('{', close_br);
            size_t next_loc   = input.find("location", close_br);
            size_t alt_close  = input.find('}', close_br + 1);
            if (depth == 1 && next_open != std::string::npos && next_open < alt_close && next_open < next_loc)
            {
                open_br  = next_open;
                close_br = input.find('}', next_open);
            } else {
                scan_pos = close_br + 1;
                if (depth == 0 || alt_close > next_loc || alt_close == std::string::npos)
                {
                    alt_close = close_br;
                }
                return trimStr(input.substr(kw_pos + 8, alt_close - kw_pos - 8));
            }
        }
    }
    if (!loc_found)
    {
        return "";
    }
    return "";
}

// loadLocationBlocks — Convierte el string de locations del .conf en objetos Location
// que el servidor almacena en _loc_list.
//
// Flujo: extractLocBlock extrae un bloque crudo → las funciones extract* sacan cada
// directiva → se construye un objeto Location con todos esos valores → se añade a _loc_list.
// El event loop consulta _loc_list en cada request para hacer longest-prefix matching
// y determinar root, métodos permitidos, CGI, límite de body, etc.
void Server::loadLocationBlocks(const std::string& location_patterns) {
    size_t      scan_pos  = 0;
    size_t      loc_found = 0;
    std::string loc_block;
    while ((loc_block = extractLocBlock(location_patterns, scan_pos, loc_found)) != "")
    {
        std::string loc_path     = extractPath(loc_block);
        std::string loc_root     = extractLocRoot(loc_block);
        bool        loc_autoindex = extractAutoindex(loc_block);
        std::string loc_return   = extractReturnVal(loc_block);
        std::string loc_try      = extractTryFiles(loc_block);
        std::string loc_cgi_path = extractCgiPath(loc_block);
        std::string loc_cgi_ext  = extractCgiExt(loc_block);
        std::string loc_redir    = extractRedirects(loc_block);
        std::string loc_index    = extractLocIndex(loc_block);
        std::string loc_methods  = extractAllowMethods(loc_block);
        // client_max_body_size en el location sobreescribe el server-level para ese path
        int         loc_max_body = extractLocMaxBodySize(loc_block);

        _loc_list.push_back(Location(loc_path, loc_root, loc_autoindex, loc_return,
                                     loc_try, loc_cgi_path, loc_cgi_ext, loc_redir,
                                     loc_index, loc_methods, loc_max_body));
    }
}

// start — Crea y configura el socket de escucha del servidor.
// Es el único socket que no tiene cliente: su trabajo es recibir conexiones nuevas.
// El event loop lo vigila con select(); cuando está listo significa que hay un
// cliente esperando ser aceptado con accept().
//
// Secuencia obligatoria para un servidor TCP no bloqueante:
//   socket() → setsockopt(SO_REUSEADDR) → fcntl(O_NONBLOCK) → bind() → listen()
//
// Devuelve el fd del socket o -1 si algún paso falla.
int Server::start() {
    int raw_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (raw_fd == -1) {
        std::cerr << "Error creating server Socket" << std::endl;
        return -1;
    }

    _sock_addr.sin_family      = AF_INET;
    _sock_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    _sock_addr.sin_port        = htons(_port);

    // SO_REUSEADDR: permite reutilizar el puerto inmediatamente después de reiniciar
    // el servidor, evitando el error "Address already in use" que causa el estado
    // TIME_WAIT del TCP (el OS reserva el puerto ~60s después de cerrar la conexión).
    int reuse_opt = 1;
    if (setsockopt(raw_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_opt, sizeof(reuse_opt)) == -1) {
        std::cerr << "Error: Socket could not be setsockopt" << std::endl;
        close(raw_fd);
        return -1;
    }
    // O_NONBLOCK: sin esto, accept() bloquearía el event loop entero esperando un cliente.
    // Con O_NONBLOCK, si no hay cliente listo accept() devuelve EAGAIN y el bucle continúa.
    // FD_CLOEXEC: cierra este fd automáticamente cuando un CGI hace execve(), evitando
    // que el proceso hijo herede el socket de escucha del servidor.
    if (fcntl(raw_fd, F_SETFL, O_NONBLOCK) == -1) {
        std::cerr << "Error setting O_NONBLOCK" << std::endl;
        close(raw_fd);
        return -1;
    }
    if (fcntl(raw_fd, F_SETFD, FD_CLOEXEC) == -1) {
        std::cerr << "Error setting FD_CLOEXEC" << std::endl;
        close(raw_fd);
        return -1;
    }
    if (bind(raw_fd, reinterpret_cast<struct sockaddr*>(&_sock_addr), sizeof(_sock_addr)) == -1) {
        std::cerr << "Unable to bind socket" << std::endl;
        close(raw_fd);
        return -1;
    }
    // backlog=128: número máximo de conexiones pendientes de aceptar en la cola del OS.
    // Con menos, bajo carga alta el OS rechazaría conexiones antes de que hagamos accept().
    int conn_backlog = 128;
    if (listen(raw_fd, conn_backlog) == -1) {
        std::cerr << "Too many pending connections" << std::endl;
        close(raw_fd);
        return -1;
    }
    return raw_fd;
}

// =============================================================================
// Helpers de streaming CGI
// =============================================================================
//
// Cuando un proceso CGI produce su respuesta, no conocemos el tamaño total de
// antemano (el CGI escribe a stdout conforme procesa). Por eso se usa
// Transfer-Encoding: chunked: cada trozo de salida se envía como un chunk con
// su tamaño al frente. El cliente va recibiendo datos sin esperar el final.
// =============================================================================

// makeChunk — Formatea un bloque de bytes como un chunk HTTP.
// Formato: "<tamaño en hex>\r\n<datos>\r\n"
// Ej: 5 bytes "hello" → "5\r\nhello\r\n"
// El event loop llama a esto cada vez que lee un bloque del stdout del CGI.
static std::string makeChunk(const char* data, size_t len) {
    std::ostringstream oss;
    oss << std::hex << len << "\r\n";
    std::string chunk = oss.str();
    chunk.append(data, len);
    chunk += "\r\n";
    return chunk;
}

// buildCGIHeaders — Convierte los headers que el CGI escribió en su stdout
// en los headers HTTP que el servidor envía al cliente.
//
// El CGI produce algo como:
//   "Status: 200 OK\r\nContent-Type: text/html\r\n\r\n<body>"
// Esta función extrae Status y Content-Type, descarta Content-Length (incompatible
// con chunked), y construye la línea de estado HTTP + headers de respuesta con
// Transfer-Encoding: chunked para poder hacer streaming del body sin conocer su tamaño.
static std::string buildCGIHeaders(const std::string& cgiHeaders) {
    std::string status      = "200";
    std::string statusText  = "OK";
    std::string contentType = "text/html; charset=utf-8";
    std::string extra;

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
                status = (e != std::string::npos) ? val.substr(s, e - s) : val.substr(s);
                if (e != std::string::npos) statusText = val.substr(e + 1);
            }
        } else if (!hline.empty() && hline.find("Content-Type:") == 0) {
            std::string val = hline.substr(13);
            size_t s = val.find_first_not_of(" ");
            if (s != std::string::npos)
                contentType = val.substr(s);
        } else if (!hline.empty() && hline.find("Content-Length:") == 0) {
            // Content-Length del CGI se descarta: usamos chunked, son mutuamente excluyentes
        } else if (!hline.empty()) {
            extra += hline + "\r\n";
        }
    }

    std::string resp = "HTTP/1.1 " + status + " " + statusText + "\r\n";
    resp += "Content-Type: " + contentType + "\r\n";
    resp += "Transfer-Encoding: chunked\r\n";
    resp += "Access-Control-Allow-Origin: *\r\n";
    resp += "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n";
    resp += "Access-Control-Allow-Headers: Content-Type\r\n";
    resp += extra;
    resp += "\r\n";
    return resp;
}

// =============================================================================
// PARTE B — Event loop: Server::run()
// =============================================================================
//
// run() es el único bucle del servidor. Nunca retorna mientras el servidor esté vivo.
// Gestiona simultáneamente todos los fds con un único select(), sin threads ni fork()
// adicionales (el único fork() permitido es el de los CGIs en startCGIAsync).
//
// Estructuras de datos que viven durante todo el bucle:
//
//   watch_set   — fd_set permanente con todos los fds activos (sockets de escucha
//                 + clientes aceptados + stdouts de CGIs). rd_set se copia de aquí
//                 antes de cada select() porque select() modifica el fd_set.
//
//   inBufs      — map<fd, string>: buffer de entrada por cliente.
//                 TCP es un stream: un request HTTP puede llegar partido en varios
//                 read(). Acumulamos aquí hasta tener headers + body completos.
//
//   sendOffsets — map<fd, size_t>: posición de envío por cliente.
//                 send() puede no enviar todo el buffer de una vez. En lugar de
//                 recortar el string con substr() (O(n²) para cuerpos grandes),
//                 guardamos un offset y pasamos c_str()+offset en el siguiente ciclo.
//
//   pendingClose — set<fd>: clientes que recibieron 413 y están en modo drenaje.
//                 No cerramos inmediatamente para evitar que el OS envíe RST antes
//                 de que el cliente reciba el 413. Seguimos leyendo y descartando
//                 hasta que el cliente cierre su lado (bytes_read <= 0).
//
//   cgi_states  — map<fd_cliente, CgiState>: estado de cada CGI en vuelo.
//                 Ciclo de vida: nace en startCGIAsync → vive mientras el hijo escribe
//                 en su stdout → muere cuando hay EOF en stdout_fd o timeout de 60s.
// =============================================================================

void Server::run(std::vector<Server>& servers) {
    fd_set rd_set, wr_set, watch_set;
    FD_ZERO(&watch_set);
    // Registrar el socket de escucha de cada servidor en watch_set.
    // select() avisará cuando haya una conexión entrante pendiente de aceptar.
    int valid = 0;
    for (size_t i = 0; i < servers.size(); i++) {
        if (servers[i].getSocket() < 0) {
            std::cerr << "Server '" << servers[i].getServerName() << "' skipped: socket invalid (port already in use?)" << std::endl;
            continue;
        }
        FD_SET(servers[i].getSocket(), &watch_set);
        valid++;
    }
    if (valid == 0) return;

    std::map<int, std::string> inBufs;
    std::map<int, size_t> sendOffsets;
    std::set<int> pendingClose;
    std::map<int, CgiState> cgi_states;


    while (g_running)
    {
        // rd_set se reconstruye desde watch_set en cada ciclo porque select()
        // sobreescribe el fd_set con solo los fds que tienen actividad.
        rd_set = watch_set;

        // wr_set se construye solo con los fds que tienen respuesta pendiente.
        // Un socket idle casi siempre tiene espacio para escribir, así que si lo
        // incluimos siempre, select() retorna inmediatamente en bucle consumiendo
        // 100% de CPU sin hacer trabajo útil.
        FD_ZERO(&wr_set);
        for (size_t i = 0; i < servers.size(); i++) {
            for (size_t j = 0; j < servers[i].cSockets.size(); j++) {
                if (!servers[i].cSockets[j].second.empty())
                    FD_SET(servers[i].cSockets[j].first, &wr_set);
            }
        }
        // Incluir también los stdin de CGIs que tienen bytes pendientes de escribir
        // al proceso hijo. Solo cuando input_written < input_buf.size() significa
        // que todavía hay body que enviar al CGI por el pipe.
        for (std::map<int, CgiState>::iterator it = cgi_states.begin();
             it != cgi_states.end(); ++it) {
            CgiState& c = it->second;
            if (!c.input_done && c.stdin_fd >= 0 &&
                c.input_written < c.input_buf.size())
                FD_SET(c.stdin_fd, &wr_set);
        }

        // select() bloquea aquí hasta que al menos un fd tenga actividad.
        // rd_set: "¿hay datos para leer?" (clientes enviando, CGI escribiendo en stdout)
        // wr_set: "¿hay espacio en el buffer del kernel para escribir?" (enviar respuesta,
        //         escribir body al pipe stdin del CGI)
        struct timeval tv;
        tv.tv_sec  = 1;
        tv.tv_usec = 0;
        if (select(FD_SETSIZE, &rd_set, &wr_set, NULL, &tv) < 0) {
            if (!g_running)
                break;
            std::cerr << "Error: select failed" << std::endl;
            break;
        }

        try {

        // ── Gestión de CGIs en vuelo ──────────────────────────────────────────
        //
        // Para cada CGI activo hay dos direcciones de datos:
        //   → stdin del CGI: el servidor escribe el body del POST al proceso hijo
        //   ← stdout del CGI: el proceso hijo escribe su respuesta HTTP, el servidor
        //                     la lee y la reenvía al cliente en chunks
        //
        // Este bloque corre antes de atender clientes nuevos para priorizar
        // que los CGIs en marcha reciban sus datos y devuelvan su respuesta.
        // ─────────────────────────────────────────────────────────────────────
        for (std::map<int, CgiState>::iterator it = cgi_states.begin();
             it != cgi_states.end(); ) {
            int       client_fd = it->first;
            CgiState& cgi       = it->second;

            // Timeout de 90s: si el CGI lleva demasiado tiempo sin terminar,
            // lo matamos con SIGKILL y respondemos 504 Gateway Timeout al cliente.
            // Sin esto, un CGI con bucle infinito bloquearía ese fd para siempre.
            if (time(NULL) - cgi.start_time > 60) {
                kill(cgi.pid, SIGKILL);
                waitpid(cgi.pid, NULL, 0);
                FD_CLR(cgi.stdout_fd, &watch_set);
                close(cgi.stdout_fd);
                if (!cgi.input_done && cgi.stdin_fd >= 0)
                    close(cgi.stdin_fd);
                for (size_t i = 0; i < servers.size(); i++) {
                    for (size_t j = 0; j < servers[i].cSockets.size(); j++) {
                        if (servers[i].cSockets[j].first == client_fd) {
                            if (cgi.headers_sent)
                                servers[i].cSockets[j].second += "0\r\n\r\n";
                            else
                                servers[i].cSockets[j].second =
                                    "HTTP/1.1 504 Gateway Timeout\r\n"
                                    "Content-Length: 0\r\nContent-Type: text/html\r\n\r\n";
                        }
                    }
                }
                cgi_states.erase(it++);
                continue;
            }

            // Escribir body al stdin del CGI.
            // input_buf contiene el body acumulado (puede haber llegado en varios
            // read() de TCP). Se escribe en chunks de 64KB para no bloquear el pipe.
            // Cuando el pipe se llena (EAGAIN) se para y se reintenta en el próximo ciclo.
            // Solo se cierra stdin cuando se escribió todo input_buf Y no quedan bytes
            // por llegar de TCP (body_remaining == 0): el CGI recibe EOF en stdin
            // y sabe que no hay más datos que procesar.
            if (!cgi.input_done && cgi.stdin_fd >= 0 &&
                FD_ISSET(cgi.stdin_fd, &wr_set)) {
                while (cgi.input_written < cgi.input_buf.size()) {
                    size_t remaining = cgi.input_buf.size() - cgi.input_written;
                    size_t chunk     = remaining < 65536 ? remaining : 65536;
                    ssize_t wr = write(cgi.stdin_fd,
                                       cgi.input_buf.c_str() + cgi.input_written,
                                       chunk);
                    if (wr > 0)
                        cgi.input_written += static_cast<size_t>(wr);
                    else
                        break; //- Si el pipe está lleno y es O_NONBLOCK → retorna -1 con EAGAIN 
                }
                if (cgi.input_written >= cgi.input_buf.size() &&
                    cgi.body_remaining == 0) {
                    close(cgi.stdin_fd);
                    cgi.stdin_fd   = -1;
                    cgi.input_done = true;
                    cgi.input_buf.clear();
                    cgi.input_written = 0;
                }
            }

            // Leer stdout del CGI y hacer streaming chunked al cliente.
            // La salida del CGI tiene formato: "<headers CGI>\r\n\r\n<body>".
            // Primero acumulamos en output_buf hasta encontrar el separador \r\n\r\n,
            // luego buildCGIHeaders() construye la respuesta HTTP y makeChunk() formatea
            // el inicio del body. Los bloques siguientes se envían directamente como chunks.
            // Cuando read() devuelve 0 (EOF) el CGI terminó: se añade el chunk terminal
            // "0\r\n\r\n" que cierra el body chunked y el cliente sabe que llegó todo.
            bool cgi_done = false;
            if (FD_ISSET(cgi.stdout_fd, &rd_set)) {
                char buf[65536];
                ssize_t n = read(cgi.stdout_fd, buf, sizeof(buf));
                if (n > 0) {
                    size_t nb = static_cast<size_t>(n);
                    if (!cgi.headers_sent) {
                        // Acumular hasta encontrar el separador headers/body del CGI
                        cgi.output_buf.append(buf, nb);
                        size_t sep = cgi.output_buf.find("\r\n\r\n");
                        size_t sep_len = 4;
                        if (sep == std::string::npos) {
                            sep = cgi.output_buf.find("\n\n");
                            sep_len = 2;
                        }
                        if (sep != std::string::npos) {
                            // Separador encontrado: construir headers HTTP + primer chunk de body
                            std::string cgi_hdrs  = cgi.output_buf.substr(0, sep);
                            std::string body_start = cgi.output_buf.substr(sep + sep_len);
                            std::string http_resp  = buildCGIHeaders(cgi_hdrs);
                            if (!body_start.empty())
                                http_resp += makeChunk(body_start.c_str(), body_start.size());
                            for (size_t si = 0; si < servers.size(); si++) {
                                for (size_t sj = 0; sj < servers[si].cSockets.size(); sj++) {
                                    if (servers[si].cSockets[sj].first == client_fd)
                                        servers[si].cSockets[sj].second = http_resp;
                                }
                            }
                            cgi.headers_sent = true;
                            cgi.output_buf.clear();
                        }
                    } else {
                        // Headers ya enviados: cada nuevo bloque de stdout se convierte
                        // directamente en un chunk y se añade al buffer de envío del cliente
                        std::string chunk = makeChunk(buf, nb);
                        for (size_t si = 0; si < servers.size(); si++) {
                            for (size_t sj = 0; sj < servers[si].cSockets.size(); sj++) {
                                if (servers[si].cSockets[sj].first == client_fd)
                                    servers[si].cSockets[sj].second += chunk;
                            }
                        }
                    }
                } else if (n == 0) {
                    cgi_done = true; // EOF: el proceso CGI cerró su stdout, terminó
                } else {
                    // n == -1: error no hay datos disponibles todavía en el pipe, en vez de bloquearse retorna -1 con errno = EAGAIN.
                }
            }

            if (cgi_done) {
                // Recoger el proceso hijo con waitpid para evitar procesos zombie.
                // Si no terminó aún (WNOHANG devuelve 0) lo matamos con SIGKILL.
                if (waitpid(cgi.pid, NULL, WNOHANG) == 0) {
                    kill(cgi.pid, SIGKILL);
                    waitpid(cgi.pid, NULL, 0);
                }
                FD_CLR(cgi.stdout_fd, &watch_set);
                close(cgi.stdout_fd);
                if (!cgi.input_done && cgi.stdin_fd >= 0)
                    close(cgi.stdin_fd);

                for (size_t si = 0; si < servers.size(); si++) {
                    for (size_t sj = 0; sj < servers[si].cSockets.size(); sj++) {
                        if (servers[si].cSockets[sj].first == client_fd) {
                            if (cgi.headers_sent) {
                                // Chunk terminal "0\r\n\r\n": indica al cliente que el
                                // body chunked está completo y puede procesar la respuesta
                                servers[si].cSockets[sj].second += "0\r\n\r\n";
                            } else {
                                // El CGI terminó sin escribir nada en stdout: error interno.
                                // buildFromCGIOutput con buffer vacío construye un 500.
                                servers[si].cSockets[sj].second =
                                    Response::buildFromCGIOutput(cgi.output_buf,
                                                                 Request(""),
                                                                 servers[si]);
                            }
                            if (cgi.close_after_send)
                                pendingClose.insert(client_fd);
                        }
                    }
                }
                cgi_states.erase(it++);
            } else {
                ++it;
            }
        }
        // ── Fin gestión CGIs ──────────────────────────────────────────────────

        for (size_t i = 0; i < servers.size(); i++) {
            // ── Nueva conexión entrante ───────────────────────────────────────
            // El socket de escucha está listo → hay un cliente esperando en la cola.
            // accept() devuelve un fd nuevo exclusivo para ese cliente.
            // Se registra en watch_set para que los ciclos siguientes lo vigilen,
            // y en cSockets para poder asociarle un buffer de respuesta.
            if (servers[i].getSocket() >= 0 && FD_ISSET(servers[i].getSocket(), &rd_set)) {
                int cli_fd = accept(servers[i].getSocket(), NULL, NULL);
                if (cli_fd < 0) {
                    std::cerr << "Error: accept: Bad file descriptor | Interrupted system call | Too many open files" << std::endl;
                    continue;
                }
                // El fd del cliente también debe ser no bloqueante: sin esto, un read()
                // o send() podría bloquear el event loop entero esperando datos de un
                // solo cliente mientras todos los demás esperan.
                fcntl(cli_fd, F_SETFL, O_NONBLOCK);
                servers[i].cSockets.push_back(std::make_pair(cli_fd, std::string("")));
                FD_SET(cli_fd, &watch_set);
            }

            // ── Lectura y procesado de clientes ───────────────────────────────
            //
            // Máquina de estados por cliente dentro de este bucle:
            //
            //  ESTADO 1 — pendingClose activo: modo drenaje
            //    El 413 ya fue enviado. Seguimos leyendo y descartando bytes para
            //    que el OS no envíe RST (lo que haría que el cliente perdiera el 413).
            //    Cuando bytes_read <= 0 el cliente cerró su lado → cierre limpio con FIN.
            //
            //  ESTADO 2 — bytes_read <= 0: cliente desconectado
            //    Limpiar todas las estructuras asociadas a este fd.
            //
            //  ESTADO 3 — bytes_read > 0: datos nuevos
            //    3a. Si hay un CGI en vuelo con body_remaining > 0: los bytes son
            //        continuación del body del POST → piparlos al CGI directamente,
            //        no procesarlos como nuevo request.
            //    3b. ¿Headers completos (\r\n\r\n)? No → continue, esperar más TCP.
            //    3c. ¿Body completo (Content-Length)? No → continue, esperar más TCP.
            //    3d. Request completo → dispatch:
            //          · 413 si body > límite (solo no-CGI)
            //          · Early CGI start: CGI con body aún llegando por TCP
            //          · CGI chunked completo: de-chunquear y lanzar CGI
            //          · Response normal: archivo estático, POST, DELETE
            // ─────────────────────────────────────────────────────────────────
            for (size_t j = 0; j < servers[i].cSockets.size(); j++)
            {
                int  close_flag = 0;
                char recv_buf[65536];
                std::memset(recv_buf, 0, 65536);
                if (FD_ISSET(servers[i].cSockets[j].first, &rd_set))
                {
                    int fd = servers[i].cSockets[j].first;

                    int bytes_read = read(fd, recv_buf, sizeof(recv_buf) - 1);

                    // ESTADO 1 — Modo drenaje: el 413 ya fue enviado a este fd.
                    // Descartamos todos los bytes entrantes. Cuando el cliente cierra
                    // su lado (bytes_read <= 0) hacemos close() limpio con FIN.
                    // Si cerráramos antes, el OS enviaría RST que podría llegar al
                    // cliente antes que el 413, causando "broken pipe" en el test siguiente.
                    if (pendingClose.count(fd)) {
                        if (bytes_read <= 0) {
                            FD_CLR(fd, &watch_set);
                            close(fd);
                            inBufs.erase(fd);
                            pendingClose.erase(fd);
                            sendOffsets.erase(fd);
                            servers[i].cSockets.erase(servers[i].cSockets.begin() + j);
                            j--;
                            continue;
                        }
                        // bytes_read > 0: datos descartados, continuar al bloque de envío
                        // por si el 413 todavía no se terminó de enviar en este ciclo.
                    } else if (bytes_read == 0) {
                        // ESTADO 2a — Cliente cerró la conexión limpiamente (FIN)
                        close_flag = 1;
                        servers[i].cSockets[j].second = "";
                        inBufs.erase(fd);
                    } else if (bytes_read < 0) {
                        // ESTADO 2b — Error de red
                        close_flag = 1;
                        servers[i].cSockets[j].second = "";
                        inBufs.erase(fd);
                    } else {
                        // ESTADO 3 — Bytes recibidos: acumular en el buffer del cliente.
                        // Se usa string(recv_buf, bytes_read) en lugar de C-string para
                        // preservar bytes nulos (0x00) que pueden aparecer en archivos binarios.
                        inBufs[fd] += std::string(recv_buf, bytes_read);

                        // ESTADO 3a — Early CGI body piping:
                        // Si este cliente ya tiene un CGI arrancado que espera más bytes
                        // del body (body_remaining > 0), los nuevos bytes van directamente
                        // al input_buf del CGI en lugar de acumularse en inBufs como request.
                        // Así el CGI recibe el body en streaming sin esperar a que llegue completo.
                        if (cgi_states.count(fd) && cgi_states[fd].body_remaining > 0) {
                            CgiState& cgi_ref = cgi_states[fd];
                            const std::string& acc = inBufs[fd];
                            size_t to_pipe = acc.size();
                            if (to_pipe > cgi_ref.body_remaining)
                                to_pipe = cgi_ref.body_remaining;
                            cgi_ref.input_buf.append(acc.c_str(), to_pipe);
                            cgi_ref.body_remaining -= to_pipe;
                            inBufs[fd].clear();
                            continue; // no procesar como nuevo request
                        }

                        std::string& inBuf = inBufs[fd];

                        // ESTADO 3b — ¿Headers completos?
                        // Los headers HTTP terminan con \r\n\r\n. Si no lo encontramos,
                        // el request llegó partido: volvemos al select() a esperar más bytes.
                        size_t headerEnd = inBuf.find("\r\n\r\n");
                        if (headerEnd == std::string::npos)
                            continue;

                        // ESTADO 3c — ¿Body completo?
                        // Content-Length indica cuántos bytes tiene el body.
                        // Si bodyReceived < contentLength, el body llegó partido por TCP:
                        // volvemos al select() a esperar más bytes (ver más abajo).
                        // Para GET no hay body (contentLength = 0), pasa directo.
                        int contentLength = 0;
                        size_t clPos = inBuf.find("Content-Length: ");
                        if (clPos != std::string::npos && clPos < headerEnd) {
                            size_t clEnd = inBuf.find("\r\n", clPos);
                            contentLength = std::atoi(inBuf.substr(clPos + 16, clEnd - clPos - 16).c_str());
                        }

                        // Detectar extensión CGI y límite de body efectivo para este request.
                        // Se recorre _loc_list buscando la location de mayor prefijo que coincide
                        // con el target (longest-prefix matching). Si esa location tiene su propio
                        // client_max_body_size, sobreescribe el límite global del servidor.
                        // isCgiExt indica si el target termina con la extensión CGI de alguna location,
                        // lo que cambia si se aplica el límite de body (los CGIs lo omiten).
                        bool isCgiExt = false;
                        int effectiveMaxBody = servers[i].getMaxBodySize();
                        {
                            size_t sp1 = inBuf.find(' ');
                            size_t sp2 = (sp1 != std::string::npos) ? inBuf.find(' ', sp1 + 1) : std::string::npos;
                            if (sp1 != std::string::npos && sp2 != std::string::npos) {
                                std::string tgt = inBuf.substr(sp1 + 1, sp2 - sp1 - 1);
                                // Eliminar query string para que la extensión se detecte correctamente
                                size_t qmark = tgt.find('?');
                                if (qmark != std::string::npos)
                                    tgt = tgt.substr(0, qmark);
                                const std::vector<Location>& locs = servers[i].getLocation();
                                int bestLen = 0;
                                for (size_t k = 0; k < locs.size(); k++) {
                                    // ¿extensión CGI?
                                    size_t dotPos = tgt.rfind('.');
                                    if (dotPos != std::string::npos &&
                                        !locs[k].getCgiExt().empty() &&
                                        locs[k].getCgiExt() == tgt.substr(dotPos))
                                        isCgiExt = true;
                                    // ¿mejor coincidencia de prefijo para max body?
                                    std::string locPath = locs[k].getLocation();
                                    if (tgt.substr(0, locPath.size()) == locPath &&
                                        static_cast<int>(locPath.size()) > bestLen) {
                                        bestLen = static_cast<int>(locPath.size());
                                        if (locs[k].getMaxBodySize() > 0)
                                            effectiveMaxBody = locs[k].getMaxBodySize();
                                    }
                                }
                            }
                        }

                        // Detectar Transfer-Encoding: chunked.
                        // Con chunked no hay Content-Length: el emisor no conoce el tamaño total.
                        // El body llega en trozos con su tamaño al frente y termina con "0\r\n\r\n".
                        // Si el terminador aún no llegó, continuamos acumulando bytes TCP.
                        // Cuando llega completo, contentLength se pone a 0 para indicar que
                        // el check de body completo de abajo no debe bloquear el procesado
                        // (el body chunked se verificará más adelante con su tamaño real decodificado).
                        bool isChunked = false;
                        size_t tePos = inBuf.find("Transfer-Encoding: chunked");
                        if (tePos == std::string::npos)
                            tePos = inBuf.find("transfer-encoding: chunked");
                        if (tePos != std::string::npos && tePos < headerEnd)
                            isChunked = true;

                        if (isChunked) {
                            size_t searchFrom = (inBuf.size() > static_cast<size_t>(bytes_read) + 4)
                                                ? inBuf.size() - static_cast<size_t>(bytes_read) - 4
                                                : headerEnd + 4;
                            if (inBuf.find("0\r\n\r\n", searchFrom) == std::string::npos)
                                continue;
                            contentLength = 0;
                        }

                        // ESTADO 3d — dispatch: el request es procesable.
                        //
                        // Primero: ¿body demasiado grande? Solo para no-CGI.
                        // Los CGIs tienen su propio check post-dechunking (línea ~813).
                        // effectiveMaxBody = 0 significa "sin límite configurado".
                        if (!isCgiExt && contentLength > effectiveMaxBody) {
                            std::string body413 = Response::readFromFile(servers[i].getRoot() + servers[i].getErrorPage(413));
                            std::ostringstream r413;
                            r413 << "HTTP/1.1 413 Content Too Large\r\nContent-Length: " << body413.size() << "\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n" << body413;
                            servers[i].cSockets[j].second = r413.str();
                            inBufs.erase(fd);
                            pendingClose.insert(fd);
                        } else {
                            // bodyReceived = bytes de body que ya llegaron (puede ser < contentLength)
                            size_t bodyReceived = inBuf.size() - (headerEnd + 4);

                            // Early CGI start: si es CGI, no chunked, y el body aún no llegó completo,
                            // arrancamos el proceso CGI ya con los bytes parciales disponibles.
                            // El resto del body llegará por TCP y el ESTADO 3a lo pipeará directamente
                            // al stdin del CGI en los ciclos siguientes, sin repasar por aquí.
                            // Esto evita acumular cuerpos de 100MB en RAM antes de empezar a procesarlos.
                            if (isCgiExt && !isChunked && (int)bodyReceived < contentLength
                                && cgi_states.count(fd) == 0) {
                                size_t remaining = static_cast<size_t>(contentLength) - bodyReceived;
                                Request partial_req(inBuf);
                                CgiState cgi = Response::startCGIAsync(
                                    partial_req, servers[i],
                                    remaining,
                                    static_cast<size_t>(contentLength));
                                if (cgi.pid > 0) {
                                    cgi.server_idx = i;
                                    bool closeConn = (inBuf.find("Connection: close") != std::string::npos);
                                    cgi.close_after_send = closeConn;
                                    cgi_states[fd] = cgi;
                                    FD_SET(cgi.stdout_fd, &watch_set);
                                }
                                inBufs.erase(fd); // los bytes parciales ya están en cgi.input_buf
                                continue;
                            }

                            // Body incompleto para no-CGI: seguir acumulando bytes TCP.
                            if (!isChunked && (int)bodyReceived < contentLength)
                                continue;

                            // ── Request completo: construir la respuesta ──────────────────
                            std::string remainder;
                            size_t consumed = headerEnd + 4 + static_cast<size_t>(contentLength);
                            bool   cgi_started = false;

                            // Pregunta 3 no existe — para cuando llegas al if (isChunked) dentro del else ya sabes que:
                            // - si era chunked → llegó completo (sino ya hizo continue arriba)
                            // - si no era chunked → o es CGI parcial, o está completo
                            if (isChunked) {
                                // De-chunking: reconstruir el body real a partir del formato chunked.
                                // Formato de cada chunk: "<tamaño hex>\r\n<datos>\r\n"
                                // El terminador final es "0\r\n\r\n".
                                // consumed marca hasta dónde llega este request en inBuf;
                                // remainder guarda lo que viene después (siguiente request keep-alive).
                                // ya que pueden recibir plusierurs request juntas
                                size_t termPos = inBuf.find("0\r\n\r\n", headerEnd + 4);
                                consumed = (termPos != std::string::npos) ? termPos + 5 : inBuf.size();
                                if (consumed < inBuf.size())
                                    remainder = inBuf.substr(consumed);

                                std::string dechunked;
                                size_t cpos = headerEnd + 4;
                                while (cpos < consumed) {
                                    size_t lfpos = inBuf.find("\r\n", cpos);
                                    if (lfpos == std::string::npos) break;
                                    size_t csz = std::strtoul(inBuf.c_str() + cpos, NULL, 16);
                                    if (csz == 0) break;
                                    cpos = lfpos + 2;
                                    if (cpos + csz > consumed) break;
                                    dechunked.append(inBuf, cpos, csz);
                                    cpos += csz + 2;
                                }

                                // Check de body size para chunked no-CGI: ahora que tenemos el body
                                // decodificado sabemos su tamaño real. Si supera el límite → 413.
                                // (Para chunked CGI este check se omite: los CGIs no tienen límite de body.)
                                if (!isCgiExt && (int)dechunked.size() > effectiveMaxBody) {
                                    std::string body413 = Response::readFromFile(servers[i].getRoot() + servers[i].getErrorPage(413));
                                    std::ostringstream r413;
                                    r413 << "HTTP/1.1 413 Content Too Large\r\nContent-Length: " << body413.size() << "\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n" << body413;
                                    servers[i].cSockets[j].second = r413.str();
                                    inBufs[fd] = remainder;
                                    pendingClose.insert(fd);
                                    continue;
                                }

                                if (isCgiExt && cgi_states.count(fd) == 0) {
                                    // Chunked CGI: el body llegó completo y ya está decodificado.
                                    // Construimos el Request solo con headers para que startCGIAsync
                                    // arranque el proceso. Luego hacemos swap del body decodificado
                                    // al input_buf del CGI en O(1) (sin copiar los 100MB).
                                    std::string hdrs_only(inBuf, 0, headerEnd + 4);
                                    Request hdr_req(hdrs_only);
                                    size_t dec_sz = dechunked.size();
                                    CgiState cgi_c = Response::startCGIAsync(
                                        hdr_req, servers[i], dec_sz, dec_sz);
                                    if (cgi_c.pid > 0) {
                                        cgi_c.input_buf.swap(dechunked); // O(1): sin copia
                                        cgi_c.body_remaining  = 0;
                                        cgi_c.server_idx      = i;
                                        cgi_c.close_after_send = (hdr_req.getConnectionType() == "close");
                                        cgi_states[fd] = cgi_c;
                                        FD_SET(cgi_c.stdout_fd, &watch_set);
                                    }
                                    if (hdr_req.getConnectionType() == "close") //nunca entraria aca debido a que en el proximo bucle estaria presente para select() y close_flag = 0
                                        close_flag = 1;
                                    inBufs[fd] = remainder;
                                    cgi_started = true;
                                } else {
                                    // Chunked no-CGI: reemplazar inBuf con la versión decodificada
                                    // para que Request y Response vean el body real, no el formato chunked.
                                    inBuf = inBuf.substr(0, headerEnd) + "\r\n\r\n" + dechunked;
                                }
                            } else {
                                // No chunked: consumed marca exactamente dónde termina este request.
                                // Si hay más bytes después son el inicio del siguiente request (keep-alive).
                                consumed = headerEnd + 4 + static_cast<size_t>(contentLength);
                            }

                            if (!cgi_started) {
                                // Construir Request y decidir si es CGI o respuesta normal.
                                // Si hay un CGI ya en cgi_states para este fd (no debería ocurrir aquí
                                // pero se comprueba por seguridad), se reutiliza ese estado.
                                // Si no es CGI (pid <= 0), Response construye directamente la respuesta
                                // (GET de archivo, POST a upload_store, DELETE, etc.).
                                Request req(inBuf);
                                CgiState cgi = (cgi_states.count(fd) == 0)
                                               ? Response::startCGIAsync(req, servers[i])
                                               : cgi_states[fd];

                                if (cgi.pid > 0 && cgi_states.count(fd) == 0) {
                                    cgi.server_idx = i;
                                    cgi.close_after_send = (req.getConnectionType() == "close");
                                    cgi_states[fd] = cgi;
                                    FD_SET(cgi.stdout_fd, &watch_set);
                                } else if (!(cgi.pid > 0)) {
                                    Response resp(req, servers[i]);
                                    servers[i].cSockets[j].second = resp.build();
                                }

                                // Avanzar inBufs al siguiente request si hay más bytes en el buffer
                                // (keep-alive: el cliente puede enviar requests sin esperar respuesta).
                                if (isChunked)
                                    inBufs[fd] = remainder;
                                else if (consumed >= inBuf.size())
                                    inBufs[fd].clear();
                                else
                                    inBufs[fd] = inBufs[fd].substr(consumed);

                                if (req.getConnectionType() == "close")
                                    close_flag = 1;
                            }
                        }
                    }
                }

                // Si se construyó una respuesta durante la lectura de este ciclo,
                // añadir el fd a wr_set ahora para enviarlo en este mismo ciclo.
                // Sin esto la respuesta esperaría al siguiente ciclo del select(),
                // donde un read() podría borrarla si el cliente envía otro request.
                if (!servers[i].cSockets[j].second.empty())
                    FD_SET(servers[i].cSockets[j].first, &wr_set);

                // ── Envío de respuesta pendiente ──────────────────────────────
                //
                // send() puede no entregar todos los bytes al kernel de una vez
                // (el buffer del socket tiene tamaño limitado). sendOffsets rastrea
                // cuántos bytes ya se entregaron: en el siguiente ciclo se reanuda
                // desde ahí con c_str() + offset, evitando copiar el buffer con substr().
                // ─────────────────────────────────────────────────────────────
                if (FD_ISSET(servers[i].cSockets[j].first, &wr_set) &&
                    !servers[i].cSockets[j].second.empty()) {
                    int    send_fd  = servers[i].cSockets[j].first;
                    size_t offset   = sendOffsets[send_fd];
                    const std::string& resp = servers[i].cSockets[j].second;

                    ssize_t sent = send(send_fd,
                         resp.c_str() + offset,
                         resp.length() - offset, 0);

                    if (sent == -1) {
                        // Error real o cliente desconectado durante el envío.
                        // Buffer lleno o error transitorio: select() reintentará.
                        // Desconexiones reales las detecta recv() en el lado de lectura.
                    } else if (sent > 0) {
                        sendOffsets[send_fd] += static_cast<size_t>(sent);
                        // Envío completo: limpiar buffer y offset
                        if (sendOffsets[send_fd] >= resp.length()) {
                            servers[i].cSockets[j].second = "";
                            sendOffsets.erase(send_fd);
                        }
                    } else {
//   send() retorna 0 solo si le pedís enviar 0 bytes:                                                                                                                                               
//   send(fd, buffer, 0, 0);  // enviás 0 bytes → retorna 0
//   Lo cual no tiene sentido hacer intencionalmente. Por eso en la práctica nunca llega al else — es código muerto. 
                        if (cgi_states.count(send_fd)) {
                            CgiState& c = cgi_states[send_fd];
                            kill(c.pid, SIGKILL);
                            waitpid(c.pid, NULL, 0);
                            FD_CLR(c.stdout_fd, &watch_set);
                            close(c.stdout_fd);
                            if (!c.input_done && c.stdin_fd >= 0)
                                close(c.stdin_fd);
                            cgi_states.erase(send_fd);
                        }
                        FD_CLR(send_fd, &watch_set);
                        close(send_fd);
                        inBufs.erase(send_fd);
                        pendingClose.erase(send_fd);
                        sendOffsets.erase(send_fd);
                        servers[i].cSockets.erase(servers[i].cSockets.begin() + j);
                        j--;
                        continue;
                    }

            }

                // Cierre limpio de conexión cuando close_flag está activo
                // (cliente pidió Connection: close, o error de lectura).
                // Las conexiones en pendingClose se cierran en el bloque de lectura
                // cuando el cliente cierra su lado, no aquí.
                
                int fd_check = servers[i].cSockets[j].first;
                if (close_flag && servers[i].cSockets[j].second.empty() && cgi_states.count(fd_check) == 0) {
                    FD_CLR(fd_check, &watch_set);
                    close(fd_check);
                    inBufs.erase(fd_check);
                    pendingClose.erase(fd_check);
                    sendOffsets.erase(fd_check);
                    servers[i].cSockets.erase(servers[i].cSockets.begin() + j);
                    j--;
                }
            }
        }
        } catch (std::exception& e) { 
            std::cerr << "Excepción en el event loop: " << e.what() << std::endl;
        }
    }

    for (size_t i = 0; i < servers.size(); i++) {
        close(servers[i].getSocket());
    }
}

// =============================================================================
// Constructores / Destructor
// =============================================================================

// Constructor de copia: necesario porque Server se almacena en std::vector.
// Cuando el vector crece y realloca, copia cada Server. _loc_iter y _loc_done
// se resetean a 0 porque son estado de iteración, no datos del servidor.
Server::Server(const Server &other)
: _sv_name(other._sv_name), _sv_host(other._sv_host), _port(other._port),
  _sv_root(other._sv_root), _max_body(other._max_body), _sv_index(other._sv_index),
  _err_map(other._err_map), _loc_list(other._loc_list),
  _loc_iter(0), _loc_done(0),
  _sock_addr(other._sock_addr), _listen_fd(other._listen_fd)
{
}

// Constructor principal: recibe los campos del bloque server{} ya parseados por
// ConfigParser, inicializa el servidor y llama a start() para crear y registrar
// el socket de escucha. Después de este constructor el servidor está listo para
// ser añadido al watch_set del event loop.
Server::Server(std::string name, std::string host, int listen, std::string root,
               int bodySize, std::string index, std::string errors, std::string locations)
: _sv_name(name), _sv_host(host), _port(listen), _sv_root(root),
  _max_body(bodySize), _sv_index(index), _loc_iter(0), _loc_done(0)
{
    this->loadErrorPages(errors);
    this->loadLocationBlocks(locations);
    this->setSocket(this->start());
}

Server &Server::operator=(const Server &other) {
    this->~Server();
    ::new(this) Server(other);
    return (*this);
}

Server::~Server() {
}

// =============================================================================
// Getters / Setters
// =============================================================================

void Server::setSocket(int n) {
    this->_listen_fd = n;
}

int Server::getSocket() const {
    return this->_listen_fd;
}

const std::vector<Location> Server::getLocation() const {
    return this->_loc_list;
}

std::string Server::getServerName() const {
    return this->_sv_name;
}

std::string Server::getHost() const {
    return this->_sv_host;
}

int Server::getListen() const {
    return this->_port;
}

std::string Server::getRoot() const {
    return this->_sv_root;
}

int Server::getMaxBodySize() const {
    return this->_max_body;
}

std::string Server::getIndex() const {
    return this->_sv_index;
}

const std::map<int, std::string>& Server::getErrorPageMap() const {
    return _err_map;
}

std::string Server::getErrorPage(int code) const {
    std::map<int, std::string>::const_iterator it = _err_map.find(code);
    if (it != _err_map.end())
        return it->second;
    else
        return "";
}



//   Caso 1: el servidor llama close(fd)
                                                                                                         
//   Cuando el servidor llama close(fd), el sistema operativo interpreta eso como:
//                                                                                                            "Este programa ya no necesita este socket para nada. Tirá todo lo que haya en el buffer y cerrá la       conexión ahora mismo."                                                                                 
//                                                                                                            Entonces lo que pasa es:                                                                                  
//   1. Servidor llama send(413)                                                                            
//            ↓      
//      [buffer del OS]  ← el 413 está acá esperando ser enviado
//                                                                                                            2. Servidor llama close(fd)
//            ↓                                                                                             
//      El OS dice: "el programa no quiere más este socket"
//            ↓                                                                                                  El OS descarta (borra, tira a la basura) los datos del buffer
//      El 413 que estaba esperando desaparece                                                              
//            ↓                                                                                                  El OS manda un paquete TCP llamado RST al cliente                                                   
//                                                                                                            3. El cliente recibe RST
//            ↓                                                                                             
//      RST significa: "la conexión fue cortada abruptamente, ignorá todo"
//            ↓                                                                                                  El cliente nunca recibe el 413
//            ↓                                                                                                  El cliente ve un error tipo "connection reset" o "broken pipe"
//                                                                                                            El 413 se perdió. El cliente no sabe qué pasó.                                                                                                                                                                  
//   ---                                                                                                    
//   Caso 2: el servidor llama shutdown(SHUT_WR)

//   shutdown(SHUT_WR) le dice al sistema operativo algo completamente diferente:
//                                                                                                            "Yo, el servidor, no voy a escribir más datos. Pero no cierres el socket todavía. Mandá al cliente todo   lo que tengas en el buffer primero, y después avisale que terminé de escribir."                       
                                                                                                         
//   Entonces lo que pasa es:

//   1. Servidor llama send(413)
//            ↓
//      [buffer del OS]  ← el 413 está acá esperando ser enviado
//                                                                                                            2. Servidor llama shutdown(SHUT_WR)
//            ↓                                                                                             
//      El OS dice: "ok, voy a mandar todo lo que tengo en el buffer
//                  y después le aviso al cliente que el servidor terminó"                                  
//            ↓                                                                                                  El OS manda el 413 al cliente  ← el dato NO se pierde                                               
//            ↓                                                                                                  El OS manda un paquete TCP llamado FIN al cliente
//      FIN significa: "el servidor terminó de escribir, pero seguís conectado"                             
//                                                                                                            3. El cliente recibe el 413 correctamente                                                                                                                                                                       
//   4. El cliente lee el 413 y entiende qué pasó                                                           

//   5. El cliente decide cerrar su lado y manda FIN de vuelta al servidor                                     
//   6. El servidor estaba haciendo read() esperando                                                             Cuando llega ese FIN, read() devuelve 0
//      Eso le avisa al servidor: "el cliente cerró su lado"                                                
//                                                                                                            7. Recién ahora el servidor llama close(fd)                                                                 En este punto el buffer ya está vacío y ambos lados cerraron                                        
//      El cierre es limpio, sin perder nada 