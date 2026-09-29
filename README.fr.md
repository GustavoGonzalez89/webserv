*Projet réalisé dans le cadre du cursus de l'École 42 Paris par gustgonz et csalaun.*

# Webserv — serveur HTTP/1.1 en C++98

🇬🇧 [English version](README.md)

## Description

Webserv est un serveur HTTP/1.1 écrit **de zéro en C++98**, sans aucune bibliothèque externe, dans un seul processus et un seul thread. Il lit un fichier de configuration inspiré de la syntaxe NGINX et sert des sites statiques, gère l'upload et la suppression de fichiers, exécute des CGI de façon asynchrone et fait tourner plusieurs serveurs virtuels à la fois. Tout est piloté par **une seule boucle d'événements basée sur `select()`**.

Projet en binôme :

| Auteur | Domaine |
|---|---|
| **gustgonz** (moi) | réseau, sockets, boucle d'événements — `Server`, `ConfigParser` |
| **csalaun** | protocole HTTP, réponses, CGI — `Request`, `Response`, `Location` |

---

## Architecture

```
                      ./webserv config/default.conf
                                   │
                  ┌────────────────▼─────────────────┐
                  │ ConfigParser                     │  un objet Server par bloc server { }
                  │  server { … location { … } }     │  + ses Location
                  └────────────────┬─────────────────┘
                                   │ Server::start() : socket → SO_REUSEADDR
                                   │   → O_NONBLOCK → bind → listen   (pour chaque serveur)
                  ┌────────────────▼─────────────────┐
                  │ Server::run() — boucle select()  │
                  │                                  │
   clients ──────▶│ fd d'écoute prêt → accept()      │
   (navigateur,   │ fd client lisible → recv(),      │──▶ Request (parsing, multipart)
    curl, siege)  │   accumulation, requête complète │──▶ Response (GET/POST/DELETE,
                  │ fd client inscriptible → send()  │    autoindex, erreurs, redirect)
                  │                                  │
                  │ pipes CGI (stdin / stdout) ──────┼──▶ fork() + execve() du script
                  │ timeout CGI 60 s → SIGKILL + 504 │    (bash, cgi_tester…)
                  └──────────────────────────────────┘
```

---

## Ce que j'ai mis en place

### Parsing de la configuration (`ConfigParser`, première partie de `Server.cpp`)
- Lecture du `.conf` (extension obligatoire), découpage en blocs `server { }` et `location { }`, commentaires `#` ignorés.
- Directives serveur : `server_name`, `host`, `listen`, `root`, `index`, `client_max_body_size` (65 536 octets par défaut), `error_page <code> <chemin>`.
- Directives location : `root`, `index`, `allow_methods`, `autoindex`, `client_max_body_size`, `redirect`, `cgi_path`, `cgi_ext`, `try_files`.
- Choix de la location par **correspondance du plus long préfixe**.

### Sockets et boucle d'événements (`Server::start`, `Server::run`)
- Pour chaque serveur virtuel : `socket()` → `setsockopt(SO_REUSEADDR)` (réutilisation immédiate du port après redémarrage) → `fcntl(O_NONBLOCK)` → `bind()` → `listen()`.
- **Un seul `select()`** surveille en même temps les sockets d'écoute, les connexions clientes et les pipes des CGI : aucune opération bloquante en dehors de `select()`.
- Tampons par client : les octets reçus sont accumulés jusqu'à une requête complète (en-têtes + corps selon `Content-Length`) ; les octets en trop sont gardés pour la requête suivante (**keep-alive**).
- Refus anticipé des corps trop gros : `413 Content Too Large` avec `Connection: close`.
- CGI asynchrone côté boucle : écriture du corps de la requête dans le stdin du CGI, lecture de sa sortie au fil de l'eau, renvoi au client en **`Transfer-Encoding: chunked`** (taille inconnue à l'avance), timeout de **60 s** puis `SIGKILL` et `504`.
- Signaux (`main.cpp`) : `SIGINT` / `SIGTERM` → arrêt propre, `SIGPIPE` ignoré (un client qui ferme ne fait pas tomber le serveur).

### Fonctionnalités du serveur (travail commun)
- `GET` (fichiers statiques, CGI, autoindex), `POST` (corps brut enregistré en `<chemin>.txt`, ou `multipart/form-data` avec le nom de fichier d'origine) → `201`, `DELETE` → `200`.
- Méthode non autorisée pour la location → `405` ; méthode inconnue → `501` ; `OPTIONS` → `204` (préflight des navigateurs).
- Autoindex généré en C++ (`opendir` / `readdir`), pages d'erreur personnalisées, redirections `301`.

---

## Lancer / tester

```bash
make                              # c++ -Wall -Wextra -Werror -std=c++98
./webserv config/default.conf     # un serveur sur 127.0.0.1:8001
./webserv config/2default.conf    # deux serveurs : 8001 et 8042
```

Résultats obtenus avec `config/default.conf` :

```bash
curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8001/                        # 200
curl -s -o /dev/null -w "%{http_code}\n" -X POST -d x http://127.0.0.1:8001/           # 405 (GET seulement)
curl -s -o /dev/null -w "%{http_code}\n" -X PATCH http://127.0.0.1:8001/               # 501
curl -X POST -d "hello world" http://127.0.0.1:8001/upload/myfile                      # 201 → upload/myfile.txt
curl http://127.0.0.1:8001/upload/myfile.txt                                           # hello world
curl -X DELETE http://127.0.0.1:8001/upload/myfile.txt                                 # 200
curl -F "file=@notes.txt" http://127.0.0.1:8001/upload/                                # 201 → upload/notes.txt
curl http://127.0.0.1:8001/upload/                                                     # listing autoindex
curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8001/git                     # 301
curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8001/cgi-bin/script.sh      # 200
head -c 20000000 /dev/zero > big.bin
curl -s -o /dev/null -w "%{http_code}\n" --data-binary @big.bin http://127.0.0.1:8001/upload/big   # 413
```

Charge : `siege -b -c 10 -r 100 http://127.0.0.1:8001/` (le sujet exige une disponibilité > 99,5 %).

Requête brute : `telnet 127.0.0.1 8001` puis `GET / HTTP/1.1`, `Host: localhost` et une ligne vide.

---

## Difficultés rencontrées

- **Tout faire passer par un seul `select()`** : les pipes des CGI devaient entrer dans la même boucle que les sockets, sinon un script lent bloquait tous les clients. D'où un état par CGI (pid, pipes, corps restant à écrire, sortie accumulée, heure de départ) consulté à chaque tour de boucle.
- **Requêtes reçues en plusieurs morceaux** : un `recv()` ne correspond pas à une requête ; il faut accumuler, détecter la fin des en-têtes puis attendre tout le corps, et garder le surplus pour la requête keep-alive suivante.
- **Sortie CGI de taille inconnue** : renvoyée en `Transfer-Encoding: chunked`, en retirant le `Content-Length` éventuel du script (les deux sont incompatibles).
- **CGI du testeur 42** : le binaire `cgi_tester` attend un corps déjà « dé-chunké » ; lui transmettre `HTTP_TRANSFER_ENCODING=chunked` le faisait échouer.
- **Préflight des navigateurs** : un upload ou un DELETE lancé depuis la page échouait quand l'origine différait (`localhost` vs `127.0.0.1`) car `OPTIONS` renvoyait 501 ; il répond maintenant 204.

---

## Compétences mobilisées

- Programmation réseau POSIX : `socket`, `bind`, `listen`, `accept`, `recv`, `send`, `setsockopt`, sockets non bloquants
- Multiplexage d'E/S avec `select()`, boucle d'événements mono-thread
- Protocole HTTP/1.1 : méthodes, codes de statut, en-têtes, keep-alive, chunked, multipart
- CGI/1.1 : `fork`, `execve`, `pipe`, variables d'environnement, gestion des processus fils et des timeouts
- Signaux UNIX, gestion des erreurs système
- C++98, Makefile, tests avec `curl`, `telnet` et `siege`
- Configuration de type NGINX (serveurs virtuels, locations)
