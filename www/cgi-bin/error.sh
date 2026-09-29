#!/bin/bash
# le dice a bash que salga inmediatamente si cualquier comando falla
set -e

# Error intencional: comando inexistente
commande_inexistante
echo "Content-Type: text/plain"
echo ""
echo "ceci n'est jamais imprimé"

# El script no imprime nada → el pipe stdout queda vacío → el servidor lee EOF y recibe cgiOut vacío → responde 500.