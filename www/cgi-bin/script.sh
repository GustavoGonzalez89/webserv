#!/bin/bash

echo "Content-Type: text/html"
echo ""
echo "<html><body><pre>"

echo "=== REQUEST INFO ==="
echo "REQUEST_METHOD: $REQUEST_METHOD"
echo "QUERY_STRING:   $QUERY_STRING"
echo "CONTENT_LENGTH: $CONTENT_LENGTH"
echo "SERVER_NAME:    $SERVER_NAME"
echo "SERVER_PORT:    $SERVER_PORT"

echo ""
echo "=== WORKING DIRECTORY (chdir check) ==="
echo "PWD: $(pwd)"
echo "Files in script dir:"
ls .
echo ""
echo "=== CAT data.txt ==="
cat data.txt
#                                   no está vacío (-n = not empty) |  CONTENT_LENGTH es mayor que 0
if [ "$REQUEST_METHOD" = "POST" ] && [ -n "$CONTENT_LENGTH" ] && [ "$CONTENT_LENGTH" -gt 0 ]; then
    echo ""
    echo "=== POST BODY ==="
    dd bs=1 count=$CONTENT_LENGTH 2>/dev/null
fi

echo "</pre></body></html>"
exit 0
