#ifndef CGI_STATE_HPP
# define CGI_STATE_HPP

#include <string>
#include <ctime>
#include <sys/types.h>

/*
** CgiState — estado de un proceso CGI en vuelo.
**
** Un CgiState se crea en startCGIAsync() y se almacena en cgi_states[client_fd].
** El event loop principal lo gestiona ciclo a ciclo hasta que el CGI termina.
**
** pid == -1  →  el request NO era CGI (startCGIAsync lo indica así).
** pid  > 0   →  proceso CGI corriendo, pipes abiertos.
*/
struct CgiState {
    pid_t       pid;            // PID del hijo CGI (-1 si no es CGI)
    int         stdin_fd;       // write end del pipe: server → CGI stdin
    int         stdout_fd;      // read  end del pipe: CGI stdout → server
    std::string input_buf;      // body completo a enviar al CGI por stdin
    size_t      input_written;  // bytes ya escritos en stdin_fd
    bool        input_done;     // true cuando stdin_fd ya fue cerrado
    std::string output_buf;     // acumula hasta encontrar el separador de headers
    bool        headers_sent;   // true una vez enviados los headers HTTP al cliente
    time_t      start_time;      // para detectar timeout (60 segundos)
    size_t      server_idx;      // índice en servers[] que atiende a este cliente
    bool        close_after_send; // true si el cliente envió Connection: close
    size_t      body_remaining;  // bytes aún esperados del TCP (0 cuando todo recibido)
};

#endif
