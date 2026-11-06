#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Servicios de Nintendo Wi-Fi Connection (lo minimo) para jugar a Monster Hunter Tri online
contra un servidor propio (MH3SP). Solo biblioteca estandar de Python 3.

Lo que pide el juego antes de llegar a los servidores de Capcom (MH3SP):
  * comprobacion de disponibilidad  UDP 27900  (<juego>.available.gs.nintendowifi.net)
  * NAS (inicio de sesion)          HTTP, puerto 9000. El juego usa https://naswii.nintendowifi.net/ac;
                                    el port lo manda aqui sin cifrar (ver port_net.cpp).
  * prueba de conexion              GET a cualquier ruta del mismo servidor web
  * GameSpy perfil (gpcm)           TCP 29900
  * GameSpy busqueda (gpsp)         TCP 29901

Los perfiles se guardan en wfc_perfiles.json (junto a este archivo).
Uso: py -3 servidor_wfc.py [--ip 0.0.0.0] [--nas 9000]

Codigo propio (MIT). El formato de los mensajes sigue lo documentado por los proyectos de
emulacion de Nintendo WFC (AltWFC/Wiimmfi).
"""

import argparse
import base64
import hashlib
import json
import logging
import os
import random
import socket
import socketserver
import string
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote

AQUI = os.path.dirname(os.path.abspath(__file__))
PERFILES = os.path.join(AQUI, "wfc_perfiles.json")

log = logging.getLogger("wfc")


def configurar_registro():
    fmt = logging.Formatter("%(asctime)s [%(name)s] %(message)s", "%H:%M:%S")
    log.setLevel(logging.DEBUG)
    h = logging.StreamHandler()
    h.setFormatter(fmt)
    h.setLevel(logging.INFO)
    log.addHandler(h)
    f = logging.FileHandler(os.path.join(AQUI, "servidor_wfc.log"), encoding="utf-8")
    f.setFormatter(fmt)
    log.addHandler(f)


def aleatorio(n, letras=string.ascii_letters + string.digits):
    return "".join(random.choice(letras) for _ in range(n))


# --------------------------------------------------------------------------- perfiles
class Perfiles:
    def __init__(self):
        self.lock = threading.Lock()
        self.datos = {"siguiente_pid": 100000001, "perfiles": {}}
        if os.path.exists(PERFILES):
            try:
                with open(PERFILES, "r", encoding="utf-8") as f:
                    self.datos = json.load(f)
            except (OSError, ValueError):
                log.warning("wfc_perfiles.json no se pudo leer; empiezo de cero")
        self.tokens = {}   # token del NAS -> datos del inicio de sesion

    def guardar(self):
        tmp = PERFILES + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.datos, f, indent=1)
        os.replace(tmp, PERFILES)

    def perfil(self, userid, gsbrcd):
        clave = "%s/%s" % (userid, gsbrcd)
        with self.lock:
            p = self.datos["perfiles"].get(clave)
            if p is None:
                pid = self.datos["siguiente_pid"]
                self.datos["siguiente_pid"] = pid + 1
                p = {
                    "profileid": pid,
                    "userid": str(userid),
                    "uniquenick": aleatorio(14, string.ascii_lowercase + string.digits) + gsbrcd[:4].lower(),
                    "gsbrcd": gsbrcd,
                }
                self.datos["perfiles"][clave] = p
                self.guardar()
                log.info("perfil nuevo %d para el usuario %s (%s)", pid, userid, gsbrcd)
            return dict(p)

    def por_pid(self, pid):
        with self.lock:
            for p in self.datos["perfiles"].values():
                if str(p["profileid"]) == str(pid):
                    return dict(p)
        return None


PERF = None


# --------------------------------------------------------------------------- NAS (HTTP)
def nas_decodificar(cuerpo):
    """Cuerpo 'clave=valor&...' con los valores en base64 (= -> *)."""
    datos = {}
    for par in cuerpo.split("&"):
        if "=" not in par:
            continue
        k, v = par.split("=", 1)
        v = unquote(v).replace("*", "=").replace("?", "/").replace(">", "+").replace("-", "/")
        try:
            datos[k] = base64.b64decode(v + "=" * (-len(v) % 4)).decode("latin-1")
        except (ValueError, base64.binascii.Error):
            datos[k] = v
    return datos


def nas_codificar(d):
    return "&".join("%s=%s" % (k, base64.b64encode(str(v).encode("latin-1")).decode().replace("=", "*"))
                    for k, v in d.items()) + "\r\n"


class Nas(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def version_string(self):
        return "Nintendo Wii (http)"

    def log_message(self, fmt, *args):
        log.debug("NAS %s " + fmt, self.client_address[0], *args)

    def responder(self, cuerpo, tipo="text/plain"):
        b = cuerpo.encode("latin-1")
        self.send_response(200)
        self.send_header("Content-type", tipo)
        self.send_header("NODE", "wifiappe1")
        self.send_header("X-Organization", "Nintendo")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        log.info("prueba de conexion: GET %s (%s)", self.path, self.headers.get("Host", "?"))
        self.responder("ok", "text/html")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0") or 0)
        cuerpo = self.rfile.read(n).decode("latin-1")
        datos = nas_decodificar(cuerpo)
        ruta = self.path.split("?")[0]
        ahora = time.strftime("%Y%m%d%H%M%S")
        if ruta == "/pr":
            palabras = "0" * len(datos.get("words", "").split("\t"))
            r = {"prwords": palabras, "returncd": "000", "datetime": ahora}
            for l in "ACEJKP":
                r["prwords" + l] = palabras
            self.responder(nas_codificar(r))
            return
        if ruta != "/ac":
            log.warning("NAS: ruta desconocida %s", self.path)
            self.send_response(404)
            self.end_headers()
            return
        accion = datos.get("action", "").lower()
        log.info("NAS %s: usuario %s, juego %s, region %s, nombre '%s'", accion, datos.get("userid", "?"),
                 datos.get("gamecd", "?"), datos.get("region", "?"), datos.get("ingamesn", ""))
        log.debug("NAS datos: %r", datos)
        if accion == "acctcreate":
            r = {"retry": "0", "returncd": "002", "userid": datos.get("userid", aleatorio(13, string.digits))}
        elif accion == "login":
            reto = aleatorio(8)
            token = "NDS" + aleatorio(80)
            PERF.tokens[token] = dict(datos, challenge=reto)
            r = {"retry": "0", "returncd": "001", "locator": "gamespy.com", "challenge": reto, "token": token}
        elif accion == "svcloc":
            token = "NDS" + aleatorio(80)
            PERF.tokens[token] = dict(datos, challenge=aleatorio(8))
            r = {"retry": "0", "returncd": "007", "statusdata": "Y", "svchost": "n/a",
                 "servicetoken": token, "token": token}
        else:
            log.warning("NAS: accion desconocida '%s'", accion)
            r = {"retry": "0", "returncd": "109"}
        r["datetime"] = ahora
        self.responder(nas_codificar(r))


# --------------------------------------------------------------------------- GameSpy
def gs_mensaje(pares):
    """[('lc', '2'), ('sesskey', '...')...] -> \\lc\\2\\sesskey\\...\\final\\"""
    return ("".join("\\%s\\%s" % (k, v) for k, v in pares) + "\\final\\").encode("latin-1")


def gs_leer(texto):
    """'\\cmd\\val\\k\\v...' -> (cmd, dict)"""
    partes = texto.split("\\")[1:]
    if not partes:
        return None, {}
    cmd = partes[0]
    d = {"__val": partes[1] if len(partes) > 1 else ""}
    for i in range(2, len(partes) - 1, 2):
        d[partes[i]] = partes[i + 1]
    return cmd, d


def md5(s):
    return hashlib.md5(s.encode("latin-1")).hexdigest()


def gs_prueba(reto_servidor, reto_nas, reto_cliente, token):
    h = md5(reto_nas)
    return md5(h + " " * 0x30 + token + reto_servidor + reto_cliente + h)


class GsBase(socketserver.BaseRequestHandler):
    nombre = "gs"

    def enviar(self, pares):
        m = gs_mensaje(pares)
        log.debug("%s -> %s", self.nombre, m)
        self.request.sendall(m)

    def bienvenida(self):
        pass

    def atender(self, cmd, d):
        log.info("%s: orden '%s' sin atender %r", self.nombre, cmd, d)

    def handle(self):
        log.info("%s: conexion de %s:%d", self.nombre, *self.client_address)
        self.request.settimeout(600)
        self.bienvenida()
        resto = b""
        try:
            while True:
                b = self.request.recv(4096)
                if not b:
                    break
                resto += b
                while b"\\final\\" in resto:
                    msg, resto = resto.split(b"\\final\\", 1)
                    texto = msg.decode("latin-1")
                    log.debug("%s <- %s", self.nombre, texto)
                    cmd, d = gs_leer(texto)
                    if cmd:
                        if self.atender(cmd, d) is False:
                            return
        except (OSError, socket.timeout) as e:
            log.info("%s: conexion cerrada (%s)", self.nombre, e)
        log.info("%s: desconectado %s:%d", self.nombre, *self.client_address)


class Gpcm(GsBase):
    nombre = "gpcm"

    def bienvenida(self):
        self.reto = aleatorio(10, string.ascii_uppercase)
        self.perfil = None
        self.sesion = random.randint(10000000, 99999999)
        self.enviar([("lc", "1"), ("challenge", self.reto), ("id", "1")])

    def atender(self, cmd, d):
        if cmd == "login":
            token = d.get("authtoken", "")
            nas = PERF.tokens.get(token)
            if nas is None:
                log.warning("gpcm: token desconocido (reinicia el juego si se reinicio el servidor)")
                self.enviar([("error", ""), ("err", "266"), ("fatal", ""),
                             ("errmsg", "There was an error validating the pre-authentication."),
                             ("id", d.get("id", "1"))])
                return
            p = PERF.perfil(nas.get("userid", "0"), nas.get("gsbrcd", nas.get("gamecd", "")))
            self.perfil = p
            prueba = gs_prueba(self.reto, nas["challenge"], d.get("challenge", ""), token)
            log.info("gpcm: sesion iniciada, perfil %d (%s)", p["profileid"], p["uniquenick"])
            self.enviar([("lc", "2"), ("sesskey", str(self.sesion)), ("proof", prueba),
                         ("userid", p["userid"]), ("profileid", str(p["profileid"])),
                         ("uniquenick", p["uniquenick"]),
                         ("lt", base64.b64encode(aleatorio(16).encode()).decode()
                          .replace("+", "[").replace("/", "]").replace("=", "_")),
                         ("id", d.get("id", "1"))])
        elif cmd == "getprofile":
            p = PERF.por_pid(d.get("profileid", "")) or self.perfil or {}
            self.enviar([("pi", ""), ("profileid", str(p.get("profileid", d.get("profileid", "0")))),
                         ("nick", p.get("uniquenick", "")), ("userid", p.get("userid", "0")),
                         ("email", "%s@nds" % p.get("uniquenick", "")), ("sig", aleatorio(32, "0123456789abcdef")),
                         ("uniquenick", p.get("uniquenick", "")), ("pid", "11"),
                         ("lon", "0.000000"), ("lat", "0.000000"), ("loc", ""), ("id", d.get("id", "1"))])
        elif cmd == "ka":
            self.enviar([("ka", "")])
        elif cmd in ("status", "updatepro", "addbuddy", "delbuddy", "authadd", "bm"):
            log.debug("gpcm: %s", cmd)
        elif cmd == "logout":
            log.info("gpcm: cierre de sesion")
            return False
        else:
            GsBase.atender(self, cmd, d)


class Gpsp(GsBase):
    nombre = "gpsp"

    def atender(self, cmd, d):
        if cmd == "otherslist":
            self.enviar([("otherslist", ""), ("oldone", "")])
        elif cmd in ("search", "searchunique"):
            self.enviar([("bsrdone", "")])
        elif cmd == "valid":
            self.enviar([("vr", "0")])
        elif cmd == "nicks":
            self.enviar([("nr", "0"), ("ndone", "")])
        else:
            GsBase.atender(self, cmd, d)
            self.enviar([("error", ""), ("err", "0"), ("id", d.get("id", "1"))])


class Disponible(socketserver.BaseRequestHandler):
    """UDP 27900: comprobacion de disponibilidad del juego y latidos del QR (se ignoran)."""

    def handle(self):
        datos, s = self.request
        if datos[:1] == b"\x09":
            juego = datos[5:].split(b"\0")[0].decode("latin-1", "replace")
            log.info("disponibilidad de '%s': si", juego)
            s.sendto(b"\xfe\xfd\x09\x00\x00\x00\x00", self.client_address)
        else:
            log.debug("qr %s: %r", self.client_address, datos[:64])


class TcpHilos(socketserver.ThreadingMixIn, socketserver.TCPServer):
    daemon_threads = True
    allow_reuse_address = True


class UdpHilos(socketserver.ThreadingMixIn, socketserver.UDPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    global PERF
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--ip", default="0.0.0.0")
    ap.add_argument("--nas", type=int, default=9000, help="puerto del NAS (online.ini: puerto_nas)")
    a = ap.parse_args()
    configurar_registro()
    PERF = Perfiles()
    servidores = []
    try:
        servidores.append(("NAS + prueba de conexion", a.nas, ThreadingHTTPServer((a.ip, a.nas), Nas)))
        servidores.append(("GameSpy perfil (gpcm)", 29900, TcpHilos((a.ip, 29900), Gpcm)))
        servidores.append(("GameSpy busqueda (gpsp)", 29901, TcpHilos((a.ip, 29901), Gpsp)))
        servidores.append(("disponibilidad (UDP)", 27900, UdpHilos((a.ip, 27900), Disponible)))
    except OSError as e:
        log.error("no se pudo abrir un puerto: %s (otro programa lo usa? ya hay otro servidor abierto?)", e)
        raise SystemExit(1)
    for nombre, puerto, srv in servidores:
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        log.info("%s escuchando en el puerto %d", nombre, puerto)
    log.info("Listo. Deja esta ventana abierta mientras juegas online (Ctrl+C para cerrar).")
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
