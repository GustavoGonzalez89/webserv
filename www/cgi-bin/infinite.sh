#!/bin/bash

# CGI con bucle infinito — no escribe nada en stdout
# El server lo mata por timeout (90s) y responde 504
while true; do
    sleep 1
done
