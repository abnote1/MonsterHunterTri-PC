#!/bin/bash
# Modo online en la Steam Deck: abre los dos servidores (servicios de Nintendo y MH3SP).
# Uso (Modo Escritorio, en Konsole):  bash servidor_online_deck.sh   (Ctrl+C para cerrarlos)
cd "$(dirname "$(readlink -f "$0")")/servidor" || exit 1
python3 servidor_wfc.py &
WFC=$!
trap 'kill $WFC 2>/dev/null' EXIT
cd MH3SP && python3 master_server.py
