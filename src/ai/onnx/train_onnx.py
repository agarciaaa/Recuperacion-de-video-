#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Untrunc-mejorado — train_onnx.py
Entrenamiento LOCAL del motor ONNX pequeño (prototipo, Fase 2.5).

Qué hace, en orden:
  1. crear-dataset: lee vídeos BUENOS y CORRUPTOS de la PC del usuario
     y extrae VENTANAS de 256 bytes con su etiqueta, sin copiar los
     vídeos enteros y sin subir nada.
  2. entrenar: entrena un MLP pequeño (264 -> 64 -> 32 -> 4) con
     PyTorch si está instalado, o con NumPy si no lo está.
  3. Exporta el modelo a .onnx real (paquete `onnx`, grafo
     Gemm/Relu/Softmax), más pesos.json (para la inferencia nativa
     en C++ sin depender de ONNX Runtime) y model_info.json.
  4. probar: mide la precisión en validación y la compara con la
     heurística actual. Si NO la supera, NO reemplaza el modelo activo.

Privacidad (garantía, endurecida 2026-11-07 a petición de Angel):
todo ocurre en la PC. El programa NUNCA copia, mueve ni escribe un
archivo de vídeo (ni completo, ni recortado, ni miniatura, ni frame
extraído) en la carpeta de entrenamiento ni en ninguna otra carpeta
del programa. Los vídeos se leen SOLO por streaming desde su lugar
original: se busca (seek) el moov para las etiquetas y se leen
ventanas de 256 bytes en el offset exacto de cada muestra; el vídeo
entero nunca se carga en memoria (antes se usaba read_bytes(), ya
eliminado) y nunca se escribe a disco. El dataset guarda solo
ventanas de 256 bytes + 8 features + etiqueta (dataset.npz) y un
manifiesto con SOLO nombres base de archivo (nunca rutas completas,
nunca EXIF/GPS, nunca audio). Las listas de la GUI viven solo en
memoria mientras la ventana está abierta y NO se persisten en disco
(antes se guardaban las rutas completas en buenos.txt/corruptos.txt;
eso se eliminó). Al final de crear-dataset y de entrenar, el script
ejecuta verificar_privacidad() sobre la carpeta de salida y falla
(código 5) si encuentra cualquier archivo de vídeo, por extensión
o por firma (magic bytes), o cualquier ruta absoluta en el manifiesto.
Nunca se sube nada.

Base persistente e incremental (§12, 2026-11-07, a petición de Angel):
la BASE que se queda es el DATASET (ventanas) + el MODELO, no las
listas de vídeos (que viven solo en memoria y se pierden al cerrar).
Al reabrir, la pestaña Entrenar muestra el resumen de la base sin
pedir vídeos. Para añadir vídeos nuevos sin recargar los anteriores:
anexar-dataset / entrenar --anexar (deduplica por hash de vídeo y
por hash de ventana; re-entrena desde cero sobre la base ampliada,
que es más estable que el fine-tuning para este MLP pequeño).
Crear-dataset desde cero BORRA la base anterior (la GUI pide
confirmación). Borrar-base exige --confirmar y vuelve al modelo
base mínimo, sin tocar los vídeos originales.

Clases (4):
  0 = basura / desconocido   1 = inicio de vídeo   2 = inicio de audio
  3 = inicio de SPS (parámetros del códec)

Ventana: 256 bytes + 8 características hechas a mano = 264 entradas.
La extracción de características DEBE ser idéntica en C++
(src/ai/onnx/onnx_provider.cpp, OnnxProvider::extractFeatures):
  f[0..255]   byte / 255.0
  f[256]      1 si los 4 primeros bytes (big-endian) son una longitud
              de NAL plausible (5..262144), si no 0
  f[257]      1 si empieza con código Annex-B (00 00 00 01 / 00 00 01)
  f[258]      tipo NAL / 31.0 (0 si no se puede determinar)
  f[259]      fracción de bytes 0x00 en la ventana
  f[260]      fracción de bytes 0xFF en la ventana
  f[261]      entropía de Shannon / 8.0
  f[262]      1 si los dos primeros bytes son 0x00 0x00
  f[263]      longitud de NAL (los 4 primeros bytes) / 262144,
              recortada a [0,1] (0 si no es plausible)

Etiquetado automático (el usuario NO etiqueta a mano):
  - Vídeo BUENO: se lee su moov (stsz/stco/co64/stsc) y se reconstruyen
    los offsets reales de cada muestra. Una ventana en el offset de
    una muestra de vídeo es "inicio_video"; de audio, "inicio_audio";
    si la primera NAL es un SPS, "inicio_sps". Ventanas aleatorias
    lejos (>=64 bytes) de cualquier inicio son "basura".
  - Vídeo CORRUPTO (sin moov): recorrido por prefijo de longitud como
    el frame_scanner de C++. Donde el recorrido encuentra un inicio de
    NAL válido es un positivo DÉBIL; donde se atasca (secuencia
    desconocida) es "basura" débil. Se marca como tal en el manifiesto.

Requisitos en la PC del usuario:
  - Python 3.10+ con numpy (para el dataset y el motor NumPy).
  - PyTorch CPU (opcional, recomendado):  pip install torch --index-url https://download.pytorch.org/whl/cpu
  - Paquete `onnx` (opcional, para exportar el .onnx):  pip install onnx
  Sin PyTorch se entrena con NumPy (más lento, mismo modelo).
  Sin `onnx` se guardan pesos.json + model_info.json y se avisa de
  que el .onnx no se exportó (el modelo sigue usándose en C++).

Uso:
  python3 train_onnx.py crear-dataset --buenos a.mp4 b.mp4 --corruptos c.mp4 --salida DIR
  python3 train_onnx.py anexar-dataset --buenos d.mp4 --salida DIR   # §12: añade sin borrar
  python3 train_onnx.py entrenar [--dataset DIR] [--salida DIR] [--motor auto|torch|numpy]
  python3 train_onnx.py entrenar --anexar --buenos d.mp4            # §12: anexa y re-entrena
  python3 train_onnx.py entrenar --buenos a.mp4 b.mp4 --corruptos c.mp4
  python3 train_onnx.py entrenar --listas DIR_ENTRENAMIENTO   # LEGADO: lee buenos.txt / corruptos.txt
  python3 train_onnx.py resumen [--carpeta DIR]               # §12: qué base queda guardada
  python3 train_onnx.py borrar-base --confirmar [--base DIR] [--destino DIR] [--dataset DIR]
  python3 train_onnx.py verificar-privacidad [--carpeta DIR_ENTRENAMIENTO]
  python3 train_onnx.py probar [--modelo DIR] [--dataset DIR]
  python3 train_onnx.py restablecer --base DIR_BASE --destino DIR_ACTIVO

Licencia: GNU GPL-2.0 (ver COPYING en la raíz del proyecto).
"""

import argparse
import hashlib
import json
import math
import os
import random
import re
import struct
import sys
import time
from pathlib import Path

# Privacidad: extensiones de vídeo que JAMÁS deben aparecer en una
# carpeta de entrenamiento, y nombres de archivo que sí se permiten
# escribir allí (dataset + modelo). Todo lo demás es sospechoso.
EXT_VIDEO = {".mp4", ".mov", ".m4v", ".3gp", ".avi", ".mkv", ".webm",
             ".flv", ".wmv", ".mpg", ".mpeg", ".m2ts", ".ts"}
ARCHIVOS_PERMITIDOS = {"dataset.npz", "dataset_manifest.json",
                       "modelo.onnx", "pesos.json", "model_info.json",
                       # Modelo de resincronizacion y aprendizaje continuo
                       # (src/aprendizaje.h): solo numeros y la firma de la
                       # camara (codec, resolucion, marca).
                       "resinc_modelo.txt", "base.tsv", "camaras.tsv",
                       "referencias.tsv", "estado.txt"}
# Texto que nunca puede aparecer en los archivos del aprendizaje continuo.
_PATRONES_RUTA = ("/home/", "/Users/", ":\\", ":/", "\\\\", "/tmp/", "/var/",
                  ".mp4", ".mov", ".m4v", ".3gp")
# Un dataset con el tope de 20.000 ejemplos ocupa como mucho
# 20.000 x 264 x 4 bytes ≈ 21 MB (comprimido, menos). Cualquier
# archivo mayor de 30 MB en la carpeta de entrenamiento no puede
# ser un dataset ni un modelo válido: es un vídeo o un error.
TAMANO_MAX_ARCHIVO = 30_000_000
# Excepcion: la base del aprendizaje continuo (base.tsv, solo numeros)
# puede llegar a 100 MB (tope en src/aprendizaje.cpp).
TAMANO_MAX_BASE = 110_000_000

VENTANA = 256
N_FEATURES = 264
CLASES = ["basura", "inicio_video", "inicio_audio", "inicio_sps"]
N_CLASES = 4
# Arquitectura del modelo base: MLP 264 -> 64 -> 32 -> 4.
# Parámetros: 264*64+64 + 64*32+32 + 32*4+4 = 19.172  (~77 KB en fp32).
ARQUITECTURA = "MLP 264->64->32->4 (ReLU, Softmax)"
PARAMETROS = 264 * 64 + 64 + 64 * 32 + 32 + 32 * 4 + 4
MAX_POR_ARCHIVO = 1500   # positivos máximos por archivo y por clase mayoritaria
MAX_EJEMPLOS = 20000     # tope del dataset (entrenar en CPU tiene que ser rápido)

try:
    import numpy as np
except ImportError:
    print("ERROR: falta numpy. Instálalo con:  pip install numpy", file=sys.stderr)
    sys.exit(2)


# ---------------------------------------------------------------------------
# Identidad de vídeos y ventanas SIN guardar rutas ni vídeos (§12)
# ---------------------------------------------------------------------------
def hash_archivo(ruta: Path):
    """Identidad de un vídeo SIN guardar su ruta ni su contenido:
    SHA-256 truncado a 12 caracteres hex, leído por streaming en
    bloques de 1 MB (el vídeo nunca se carga entero en memoria).
    Sirve para saber si un vídeo ya aportó a la base sin tener que
    guardar dónde estaba. Dos vídeos con el mismo contenido dan el
    mismo hash aunque se llamen distinto o estén en otra carpeta."""
    h = hashlib.sha256()
    try:
        with open(ruta, "rb") as f:
            while True:
                trozo = f.read(1 << 20)
                if not trozo:
                    break
                h.update(trozo)
    except OSError:
        return ""
    return h.hexdigest()[:12]


def hash_ventana(fila: np.ndarray):
    """Huella de una ventana ya extraída (fila de 264 float32).
    Dos ventanas idénticas dan la misma huella: así se deduplica
    al anexar sin guardar el vídeo ni el offset del que salió."""
    return hashlib.sha256(np.ascontiguousarray(fila).tobytes()).hexdigest()[:12]


def _es_nombre_base_valido(nombre: str):
    return bool(nombre) and "/" not in nombre and "\\" not in nombre \
        and nombre not in (".", "..") and not os.path.isabs(nombre)


def _es_hash_valido(h):
    return isinstance(h, str) and len(h) == 12 \
        and all(c in "0123456789abcdef" for c in h)


# ---------------------------------------------------------------------------
# Características (idénticas a C++: onnx_provider.cpp extractFeatures)
# ---------------------------------------------------------------------------
def extraer_features(ventana: bytes) -> np.ndarray:
    w = bytes(ventana[:VENTANA])
    if len(w) < VENTANA:
        w = w + b"\x00" * (VENTANA - len(w))
    f = np.zeros(N_FEATURES, dtype=np.float32)
    f[:VENTANA] = np.frombuffer(w, dtype=np.uint8).astype(np.float32) / 255.0
    be32 = struct.unpack(">I", w[:4])[0] if len(w) >= 4 else 0
    plausible = 5 <= be32 <= 262144
    f[256] = 1.0 if plausible else 0.0
    f[257] = 1.0 if (w[:4] == b"\x00\x00\x00\x01" or w[:3] == b"\x00\x00\x01") else 0.0
    nal_tipo = 0
    if plausible and len(w) >= 5:
        nal_tipo = w[4] & 0x1F
    elif f[257] > 0:
        off = 4 if w[:4] == b"\x00\x00\x00\x01" else 3
        if len(w) > off:
            nal_tipo = w[off] & 0x1F
    f[258] = nal_tipo / 31.0
    f[259] = w.count(0) / VENTANA
    f[260] = w.count(0xFF) / VENTANA
    counts = [0] * 256
    for b in w:
        counts[b] += 1
    ent = 0.0
    for c in counts:
        if c:
            p = c / VENTANA
            ent -= p * math.log2(p)
    f[261] = ent / 8.0
    f[262] = 1.0 if (len(w) >= 2 and w[0] == 0 and w[1] == 0) else 0.0
    f[263] = min(be32 / 262144.0, 1.0) if plausible else 0.0
    return f


def clasificador_regla(f: np.ndarray) -> int:
    """Heurística actual, como clasificador, para la línea base.
    Devuelve la clase que predeciría solo con reglas (sin red)."""
    if f[256] > 0.5:  # longitud de NAL plausible
        tipo = int(round(f[258] * 31))
        if tipo in (7, 33):
            return 3
        if tipo in (1, 5, 0, 19, 20):
            return 1
    return 0


# ---------------------------------------------------------------------------
# Parseo mínimo de MP4 (solo stdlib): moov -> muestras por track
# ---------------------------------------------------------------------------
def _leer_atomos(buf: bytes, inicio: int, fin: int, profundidad=0):
    """Genera (nombre, inicio_contenido, fin) de cada átomo hijo."""
    off = inicio
    while off + 8 <= fin and profundidad < 8:
        tam = struct.unpack(">I", buf[off:off + 4])[0]
        nombre = buf[off + 4:off + 8].decode("latin-1", "replace")
        if tam == 1 and off + 16 <= fin:
            tam = struct.unpack(">Q", buf[off + 8:off + 16])[0]
            cab = 16
        elif tam == 0:
            tam = fin - off
            cab = 8
        else:
            cab = 8
        if tam < cab or off + tam > fin:
            break
        yield nombre, off + cab, off + tam
        off += tam


def _buscar(buf: bytes, inicio: int, fin: int, ruta):
    """Busca átomos por ruta, p. ej. ['moov','trak','mdia','minf','stbl','stsz']."""
    if not ruta:
        return [(inicio, fin)]
    nombre = ruta[0]
    out = []
    for n, c0, c1 in _leer_atomos(buf, inicio, fin):
        if n == nombre:
            out.extend(_buscar(buf, c0, c1, ruta[1:]))
    return out


def _u32(buf, off):
    return struct.unpack(">I", buf[off:off + 4])[0]


def _u64(buf, off):
    return struct.unpack(">Q", buf[off:off + 8])[0]


# ---------------------------------------------------------------------------
# Lectura por streaming (privacidad): el vídeo NUNCA se carga entero
# en memoria ni se copia a disco. Se abre en su lugar original, se
# busca (seek) el moov para las etiquetas y se leen solo ventanas de
# 256 bytes en el offset exacto de cada muestra.
# ---------------------------------------------------------------------------
def _atomos_raiz_stream(f, tamano_archivo):
    """Genera (nombre, inicio_contenido, fin) de los átomos raíz,
    leyendo solo sus cabeceras de 8/16 bytes por streaming."""
    off = 0
    while off + 8 <= tamano_archivo:
        f.seek(off)
        cab = f.read(8)
        if len(cab) < 8:
            break
        tam = struct.unpack(">I", cab[:4])[0]
        nombre = cab[4:8].decode("latin-1", "replace")
        cabecera = 8
        if tam == 1:
            ext = f.read(8)
            if len(ext) < 8:
                break
            tam = struct.unpack(">Q", ext)[0]
            cabecera = 16
        elif tam == 0:
            tam = tamano_archivo - off
        if tam < cabecera or off + tam > tamano_archivo:
            break
        yield nombre, off + cabecera, off + tam
        off += tam


def _leer_ventana(f, offset):
    """Lee como mucho 256 bytes en un offset exacto, por streaming.
    Devuelve b'' si el offset está fuera del archivo."""
    try:
        f.seek(offset)
    except OSError:
        return b""
    return f.read(VENTANA)


def verificar_privacidad(carpeta: Path):
    """Escanea una carpeta de entrenamiento y devuelve la lista de
    problemas de privacidad (vacía = todo bien). Comprueba:
      * ningún archivo con extensión de vídeo;
      * ningún archivo con firma de vídeo (ftyp/moov/mdat en los
        primeros 16 bytes) ni mayor de TAMANO_MAX_ARCHIVO;
      * ningún archivo fuera de la lista permitida;
      * el manifiesto no contiene rutas absolutas ni separadores
        de ruta (solo nombres base);
      * dataset.npz solo tiene X (N,264) e y (N,) — nunca ventanas
        mayores de 256 bytes ni segmentos continuos.
    No escribe nada: solo lee y reporta."""
    problemas = []
    carpeta = Path(carpeta)
    if not carpeta.exists():
        return [f"la carpeta {carpeta} no existe"]
    for raiz, _dirs, archivos in os.walk(carpeta):
        for nombre in archivos:
            p = Path(raiz) / nombre
            if p.suffix.lower() in EXT_VIDEO:
                problemas.append(f"archivo de vídeo en la carpeta de "
                                 f"entrenamiento: {p.name} (extensión "
                                 f"{p.suffix})")
                continue
            if nombre not in ARCHIVOS_PERMITIDOS:
                problemas.append(f"archivo no esperado en la carpeta de "
                                 f"entrenamiento: {p.name}")
            try:
                tam = p.stat().st_size
            except OSError as e:
                problemas.append(f"no se pudo revisar {p.name}: {e}")
                continue
            limite = TAMANO_MAX_BASE if nombre == "base.tsv" else TAMANO_MAX_ARCHIVO
            if tam > limite:
                problemas.append(f"archivo sospechosamente grande "
                                 f"({tam} bytes > {limite}): "
                                 f"{p.name} — no puede ser un dataset ni "
                                 f"un modelo válido")
            try:
                with open(p, "rb") as fh:
                    cabecera = fh.read(16)
            except OSError:
                continue
            if b"ftyp" in cabecera or cabecera[4:8] in (b"moov", b"mdat",
                                                       b"free", b"skip"):
                problemas.append(f"firma de vídeo (ftyp/moov/mdat) en "
                                 f"los primeros bytes de {p.name}: no es "
                                 f"un dataset ni un modelo")
            if p.suffix.lower() in (".tsv", ".txt") and tam <= limite:
                try:
                    texto = p.read_text(encoding="utf-8", errors="replace")
                except OSError:
                    texto = ""
                for patron in _PATRONES_RUTA:
                    if patron in texto.lower():
                        problemas.append(f"{p.name} contiene una ruta o un nombre de "
                                         f"vídeo ({patron!r}): solo se permiten números")
                        break
    manifiesto = carpeta / "dataset" / "dataset_manifest.json"
    if not manifiesto.exists():
        manifiesto = carpeta / "dataset_manifest.json"
    if manifiesto.exists():
        try:
            texto = manifiesto.read_text(encoding="utf-8")
        except OSError as e:
            problemas.append(f"no se pudo leer el manifiesto: {e}")
            texto = ""
        for patron in ("/home/", "/Users/", "C:\\", "C:/", "D:\\", "D:/",
                       "\\\\", "/tmp/", "/var/"):
            if patron in texto:
                problemas.append(f"el manifiesto contiene una ruta "
                                 f"absoluta o de sistema ({patron!r}): "
                                 f"solo se permiten nombres base")
                break
        if re.search(r'"nombre":\s*"[^"]*[/\\]', texto):
            problemas.append("el manifiesto guarda un nombre de archivo "
                             "con separador de ruta: solo se permite el "
                             "nombre base (p. ej. ref.mp4)")
        # §12: nunca una clave "ruta" en el manifiesto, y cada entrada
        # de "archivos" debe tener solo nombre base + hash de 12 hex.
        if '"ruta"' in texto:
            problemas.append("el manifiesto contiene una clave \"ruta\": "
                             "las rutas de los vídeos nunca se guardan")
        try:
            man = json.loads(texto)
            for ent in man.get("archivos", []):
                if not _es_nombre_base_valido(ent.get("nombre", "")):
                    problemas.append(f"el manifiesto guarda un nombre no "
                                     f"válido como nombre base: "
                                     f"{ent.get('nombre')!r}")
                    break
                if "hash" in ent and not _es_hash_valido(ent["hash"]):
                    problemas.append(f"el manifiesto guarda un hash no "
                                     f"válido para {ent.get('nombre')!r}: "
                                     f"solo se permite un SHA-256 truncado "
                                     f"a 12 hex")
                    break
        except json.JSONDecodeError:
            problemas.append("el manifiesto no es un JSON válido")
    npz = carpeta / "dataset" / "dataset.npz"
    if not npz.exists():
        npz = carpeta / "dataset.npz"
    if npz.exists():
        try:
            d = np.load(npz)
            claves = set(d.files)
            if claves != {"X", "y"}:
                problemas.append(f"dataset.npz con claves no esperadas "
                                 f"{sorted(claves)} (solo se permiten X e y)")
            else:
                if len(d["X"].shape) != 2 or d["X"].shape[1] != N_FEATURES:
                    problemas.append(f"dataset.npz: X tiene forma "
                                     f"{d['X'].shape}, se esperaba (N, "
                                     f"{N_FEATURES}) — una ventana mayor de "
                                     f"256 bytes no cabe aquí")
                if len(d["y"].shape) != 1 or len(d["y"]) != len(d["X"]):
                    problemas.append("dataset.npz: y no tiene un elemento "
                                     "por cada ventana de X")
        except Exception as e:  # noqa: BLE001 - el .npz puede estar corrupto
            problemas.append(f"no se pudo validar dataset.npz: {e}")
    return problemas


def muestras_de_video_bueno(ruta: Path):
    """Devuelve (muestras, mdat_inicio, mdat_fin, error_es).
    muestras: lista de (offset, clase, codec) con etiquetas REALES
    sacadas del moov. Vacía + error si no hay moov.
    Lectura por streaming: solo se leen las cabeceras de los átomos
    raíz y el bloque moov (metadatos, no vídeo); el mdat nunca se
    lee entero y el archivo nunca se copia."""
    try:
        f = open(ruta, "rb")
    except OSError as e:
        return [], 0, 0, f"No se pudo leer {ruta.name}: {e}"
    with f:
        f.seek(0, 2)
        tamano = f.tell()
        moov = None
        mdat = None
        for n, c0, c1 in _atomos_raiz_stream(f, tamano):
            if n == "moov":
                moov = (c0, c1)
            elif n == "mdat":
                mdat = (c0, c1)
        if moov is None:
            return [], 0, 0, "sin moov (archivo dañado o incompleto)"
        if mdat is None:
            return [], 0, 0, "sin mdat (no hay datos de vídeo)"
        # Solo el moov (índice, no vídeo) se lee a memoria.
        f.seek(moov[0])
        moov_bytes = f.read(moov[1] - moov[0])
        muestras = []
        # Cada trak: stsd (códec) + stsz + stco/co64 + stsc.
        # _buscar devuelve offsets relativos al inicio del moov.
        for t0, t1 in _buscar(moov_bytes, 0, len(moov_bytes), ["trak"]):
            stsd = _buscar(moov_bytes, t0, t1, ["mdia", "minf", "stbl", "stsd"])
            codec = "?"
            if stsd:
                s0, s1 = stsd[0]
                if s1 - s0 >= 16:
                    codec = moov_bytes[s0 + 12:s0 + 16].decode("latin-1", "replace")
            if codec in ("avc1", "hvc1", "hev1", "mp4v"):
                clase = 1
            elif codec in ("mp4a", "sawb", "sowt", "twos"):
                clase = 2
            else:
                continue  # metadatos, subtítulos... no se usan como ejemplo
            stsz = _buscar(moov_bytes, t0, t1, ["mdia", "minf", "stbl", "stsz"])
            stco = _buscar(moov_bytes, t0, t1, ["mdia", "minf", "stbl", "stco"])
            co64 = _buscar(moov_bytes, t0, t1, ["mdia", "minf", "stbl", "co64"])
            stsc = _buscar(moov_bytes, t0, t1, ["mdia", "minf", "stbl", "stsc"])
            if not stsz or (not stco and not co64) or not stsc:
                continue
            z0, z1 = stsz[0]
            tam_const = _u32(moov_bytes, z0 + 4)
            n_muestras = _u32(moov_bytes, z0 + 8)
            tamanos = []
            if tam_const != 0:
                tamanos = [tam_const] * n_muestras
            else:
                for i in range(n_muestras):
                    tamanos.append(_u32(moov_bytes, z0 + 12 + 4 * i))
            if co64:
                c0, c1 = co64[0]
                n_chunks = _u32(moov_bytes, c0 + 4)
                chunks = [_u64(moov_bytes, c0 + 8 + 8 * i) for i in range(n_chunks)]
            else:
                c0, c1 = stco[0]
                n_chunks = _u32(moov_bytes, c0 + 4)
                chunks = [_u32(moov_bytes, c0 + 8 + 4 * i) for i in range(n_chunks)]
            s0, s1 = stsc[0]
            n_entr = _u32(moov_bytes, s0 + 4)
            entradas = []  # (primer_chunk_1based, muestras_por_chunk)
            for i in range(n_entr):
                base = s0 + 8 + 12 * i
                entradas.append((_u32(moov_bytes, base), _u32(moov_bytes, base + 4)))
            idx = 0
            for k, chunk_off in enumerate(chunks):
                e = entradas[0]
                for cand in entradas:
                    if cand[0] <= k + 1:
                        e = cand
                    else:
                        break
                off = chunk_off
                for _ in range(e[1]):
                    if idx >= n_muestras:
                        break
                    muestras.append((off, clase, codec))
                    off += tamanos[idx]
                    idx += 1
        # Clase SPS: ventana de una muestra de vídeo cuya primera NAL
        # con prefijo de longitud es un SPS (tipo 7 H.264 / 33 HEVC).
        # Se lee solo esa ventana de 5 bytes, por streaming.
        refinadas = []
        for off, clase, codec in muestras:
            if clase == 1 and mdat[0] <= off < mdat[1]:
                f.seek(off)
                cab = f.read(5)
                if len(cab) >= 5:
                    ln = _u32(cab, 0)
                    if 5 <= ln <= 262144:
                        tipo = cab[4] & 0x1F
                        if tipo in (7, 33):
                            clase = 3
            refinadas.append((off, clase, codec))
        return refinadas, mdat[0], mdat[1], ""


def ventanas_de_video_corrupto(ruta: Path):
    """Etiquetado DÉBIL para un vídeo sin moov (ver cabecera).
    Devuelve (muestras, mdat_inicio, mdat_fin, error_es).
    Lectura por streaming en bloques de 1 MB: el mdat se recorre
    sin cargarlo entero en memoria y el archivo nunca se copia."""
    try:
        f = open(ruta, "rb")
    except OSError as e:
        return [], 0, 0, f"No se pudo leer {ruta.name}: {e}"
    with f:
        f.seek(0, 2)
        tamano = f.tell()
        mdat = None
        for n, c0, c1 in _atomos_raiz_stream(f, tamano):
            if n == "mdat":
                mdat = (c0, c1)
                break
        if mdat is None:
            return [], 0, 0, "sin mdat localizable"
        muestras = []
        atascos = []
        fin = min(mdat[1], mdat[0] + (64 << 20))  # como el frame_scanner: 64 MB
        # Recorrido por prefijo de longitud sobre un búfer deslizante:
        # se lee 1 MB, se procesa, y solo queda la cola sin procesar.
        f.seek(mdat[0])
        buf = bytearray()
        base = mdat[0]  # offset de archivo del primer byte de buf
        eof = False
        pasos = 0
        while pasos < 2000000:
            while len(buf) < 5 and not eof:
                trozo = f.read(1 << 20)
                if not trozo:
                    eof = True
                    break
                buf.extend(trozo)
            if len(buf) < 5:
                break
            pasos += 1
            off = base
            ln = struct.unpack(">I", bytes(buf[:4]))[0]
            if 5 <= ln <= 1048576:
                tipo = buf[4] & 0x1F
                if tipo in (1, 5, 19, 20):
                    muestras.append((off, 1, "avc1?"))
                    salto = 4 + ln
                    if salto <= len(buf):
                        del buf[:salto]
                        base += salto
                    else:
                        # El frame es más grande que el búfer: se
                        # salta en el archivo sin cargarlo (seek
                        # relativo; la posición del archivo es
                        # base + len(buf)).
                        f.seek(salto - len(buf), 1)
                        buf.clear()
                        base += salto
                    continue
                if tipo in (7, 33):
                    muestras.append((off, 3, "avc1?"))
                    salto = 4 + ln
                    if salto <= len(buf):
                        del buf[:salto]
                        base += salto
                    else:
                        f.seek(salto - len(buf), 1)
                        buf.clear()
                        base += salto
                    continue
            atascos.append(off)
            del buf[:1]
            base += 1
            if base >= fin and eof:
                break
            if base >= fin:
                break
        for off in atascos[:500]:
            muestras.append((off, 0, "?"))
        return muestras, mdat[0], mdat[1], ""


# ---------------------------------------------------------------------------
# Dataset
# ---------------------------------------------------------------------------
def extraer_ejemplos(buenos, corruptos, max_por_archivo=MAX_POR_ARCHIVO):
    """Extrae ventanas (features) + etiquetas de vídeos buenos y
    corruptos, por streaming y sin copiar nada. Devuelve
    (X_lista, y_lista, entradas_manifiesto, n_debiles).
    Cada entrada del manifiesto lleva SOLO nombre base + hash
    SHA-256 truncado (12 hex) + nº de ejemplos + fecha: nunca la
    ruta. Es la pieza que comparten crear-dataset (desde cero) y
    anexar-dataset (incremental, §12)."""
    X, y = [], []
    entradas = []
    debil = 0
    rng = random.Random(1234)
    for ruta in buenos:
        muestras, m0, m1, err = muestras_de_video_bueno(Path(ruta))
        # Solo el nombre base, nunca la ruta completa (privacidad).
        info = {"nombre": Path(ruta).name, "hash": hash_archivo(Path(ruta)),
                "tipo": "bueno", "error": err, "muestras": len(muestras),
                "fecha": time.strftime("%Y-%m-%dT%H:%M:%S")}
        if err or not muestras:
            entradas.append(info)
            print(f"  [bueno] {Path(ruta).name}: {err or 'sin muestras'}")
            continue
        por_clase = {}
        for off, clase, codec in muestras:
            por_clase.setdefault(clase, []).append(off)
        elegidas = []
        for clase, offs in por_clase.items():
            rng.shuffle(offs)
            elegidas.extend((o, clase) for o in offs[:max_por_archivo])
        # Negativos: offsets aleatorios lejos de cualquier inicio.
        inicios = sorted(o for o, _, _ in muestras)
        neg = 0
        intentos = 0
        objetivo_neg = min(len(elegidas), max_por_archivo)
        while neg < objetivo_neg and intentos < objetivo_neg * 20:
            intentos += 1
            o = rng.randrange(m0, max(m0 + 1, m1 - VENTANA))
            import bisect
            i = bisect.bisect_left(inicios, o)
            cerca = False
            for j in (i - 1, i):
                if 0 <= j < len(inicios) and abs(inicios[j] - o) < 64:
                    cerca = True
            if not cerca:
                elegidas.append((o, 0))
                neg += 1
        # Ventanas por streaming: se abre el archivo original en su
        # lugar y se lee solo la ventana de 256 bytes de cada offset.
        with open(ruta, "rb") as f:
            for off, clase in elegidas:
                X.append(extraer_features(_leer_ventana(f, off)))
                y.append(clase)
        info["ejemplos"] = len(elegidas)
        entradas.append(info)
        print(f"  [bueno] {Path(ruta).name}: {len(elegidas)} ejemplos")
    for ruta in corruptos:
        muestras, m0, m1, err = ventanas_de_video_corrupto(Path(ruta))
        info = {"nombre": Path(ruta).name, "hash": hash_archivo(Path(ruta)),
                "tipo": "corrupto_debil", "error": err, "muestras": len(muestras),
                "fecha": time.strftime("%Y-%m-%dT%H:%M:%S")}
        if err or not muestras:
            entradas.append(info)
            print(f"  [corrupto] {Path(ruta).name}: {err or 'sin muestras'}")
            continue
        rng.shuffle(muestras)
        # Ventanas por streaming, igual que en los buenos.
        with open(ruta, "rb") as f:
            n = 0
            for off, clase, _ in muestras[:max_por_archivo]:
                X.append(extraer_features(_leer_ventana(f, off)))
                y.append(clase)
                debil += 1
                n += 1
        info["ejemplos"] = n
        entradas.append(info)
        print(f"  [corrupto] {Path(ruta).name}: {n} ejemplos (etiqueta débil)")
    return X, y, entradas, debil


def _manifiesto_base():
    return {"version": 1, "ventana": VENTANA, "features": N_FEATURES,
            "clases": CLASES, "archivos": [], "ejemplos_por_clase": [0] * N_CLASES,
            "debil": 0, "total": 0,
            "fecha": time.strftime("%Y-%m-%dT%H:%M:%S"),
            "actualizado": time.strftime("%Y-%m-%dT%H:%M:%S"),
            "privacidad": {
                "guarda_videos": False,
                "ventana_bytes": VENTANA,
                "rutas_guardadas": "solo nombres base + hash SHA-256 truncado "
                                   "(12 hex, p. ej. ref.mp4 abc123...), nunca "
                                   "rutas completas, nunca EXIF/GPS, nunca audio",
                "lectura": "streaming por seek desde el archivo original, "
                           "en su lugar; el vídeo nunca se copia ni se "
                           "carga entero en memoria"}}


def _guardar_dataset(salida: Path, X, y, manifiesto):
    """Escribe dataset.npz + manifiesto y verifica privacidad.
    Devuelve 0 si todo bien, 5 si la verificación falla."""
    salida.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(salida / "dataset.npz", X=X, y=y)
    (salida / "dataset_manifest.json").write_text(
        json.dumps(manifiesto, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"Dataset guardado en {salida}: {len(y)} ejemplos "
          f"(por clase: {manifiesto['ejemplos_por_clase']}).")
    print("Solo se guardaron ventanas de 256 bytes + etiquetas; "
          "los vídeos enteros NO se copiaron.")
    problemas = verificar_privacidad(salida)
    if problemas:
        print("ERROR DE PRIVACIDAD: el dataset no es seguro, no se usará:",
              file=sys.stderr)
        for p in problemas:
            print(f"  - {p}", file=sys.stderr)
        return 5
    print("Privacidad verificada: en la carpeta del dataset no hay "
          "ningún vídeo, ni rutas completas en el manifiesto.")
    return 0


def crear_dataset(buenos, corruptos, salida: Path, max_por_archivo=MAX_POR_ARCHIVO):
    """Crea el dataset DESDE CERO: borra la base anterior. La GUI
    pide confirmación antes de llamar aquí (ver gui.cpp); para
    añadir vídeos sin borrar nada, usar anexar_dataset (§12)."""
    X, y, entradas, debil = extraer_ejemplos(buenos, corruptos, max_por_archivo)
    if not X:
        print("ERROR: no se extrajo ningún ejemplo. Añade al menos un vídeo "
              "bueno que se reproduzca completo.", file=sys.stderr)
        return 1
    rng = random.Random(1234)
    idx = list(range(len(X)))
    rng.shuffle(idx)
    idx = idx[:MAX_EJEMPLOS]
    X = np.stack([X[i] for i in idx]).astype(np.float32)
    y = np.array([y[i] for i in idx], dtype=np.int64)
    manifiesto = _manifiesto_base()
    manifiesto["archivos"] = entradas
    manifiesto["debil"] = debil
    for c in y:
        manifiesto["ejemplos_por_clase"][int(c)] += 1
    manifiesto["total"] = int(len(y))
    return _guardar_dataset(salida, X, y, manifiesto)


def _cargar_dataset_existente(salida: Path):
    """Devuelve (X, y, manifiesto) del dataset guardado, o
    (None, None, None) si no hay base todavía."""
    npz_p = salida / "dataset.npz"
    man_p = salida / "dataset_manifest.json"
    if not npz_p.exists() or not man_p.exists():
        return None, None, None
    try:
        d = np.load(npz_p)
        manifiesto = json.loads(man_p.read_text(encoding="utf-8"))
    except Exception as e:  # noqa: BLE001
        print(f"ERROR: la base guardada está corrupta ({e}); crea el "
              f"dataset desde cero.", file=sys.stderr)
        return None, None, None
    return d["X"], d["y"], manifiesto


def anexar_dataset(buenos, corruptos, salida: Path, max_por_archivo=MAX_POR_ARCHIVO):
    """Entrenamiento INCREMENTAL (§12): añade vídeos nuevos a la
    base guardada SIN borrarla y SIN volver a cargar los vídeos
    anteriores (sus ventanas ya están en dataset.npz).
    - Si un vídeo ya aportó (mismo nombre base + mismo hash), se
      salta entero y se dice: no se duplica.
    - Las ventanas nuevas se deduplican por hash de ventana contra
      las que ya hay: si agregas el mismo vídeo dos veces, el
      total no crece.
    - Tope global 20.000 ejemplos: si se supera, se quedan los
      viejos y se añaden nuevos hasta el tope, y se avisa.
    Devuelve 0 si se anexó algo (o nada nuevo que anexar, rc 0 con
    aviso), 1 si no se extrajo nada nuevo, 5 si falla privacidad."""
    X_viejo, y_viejo, manifiesto = _cargar_dataset_existente(salida)
    if X_viejo is None:
        print("No hay base guardada todavía: se crea el dataset desde "
              "cero con estos vídeos.")
        return crear_dataset(buenos, corruptos, salida, max_por_archivo)
    # Vídeos que ya aportaron: (nombre base, hash) -> se saltan.
    ya = {(e.get("nombre"), e.get("hash")) for e in manifiesto.get("archivos", [])}
    buenos_n = [b for b in buenos
                if (Path(b).name, hash_archivo(Path(b))) not in ya]
    corruptos_n = [c for c in corruptos
                   if (Path(c).name, hash_archivo(Path(c))) not in ya]
    saltados = (len(buenos) - len(buenos_n)) + (len(corruptos) - len(corruptos_n))
    if saltados:
        print(f"  {saltados} vídeo(s) ya habían aportado a la base "
              f"(mismo nombre y mismo contenido): se saltan, no se duplican.")
    if not buenos_n and not corruptos_n:
        print("Nada nuevo que anexar: todos los vídeos ya estaban en la base. "
              f"La base sigue con {manifiesto.get('total', len(y_viejo))} ejemplos.")
        return 0
    X_nuevo, y_nuevo, entradas, debil = extraer_ejemplos(
        buenos_n, corruptos_n, max_por_archivo)
    if not X_nuevo:
        print("ERROR: no se extrajo ningún ejemplo de los vídeos nuevos.",
              file=sys.stderr)
        return 1
    # Deduplicación por hash de ventana contra la base existente.
    vistas = {hash_ventana(f) for f in X_viejo}
    keep = []
    for i, f in enumerate(X_nuevo):
        h = hash_ventana(f)
        if h in vistas:
            continue
        vistas.add(h)
        keep.append(i)
    print(f"  Ventanas nuevas: {len(X_nuevo)} extraídas, "
          f"{len(keep)} realmente nuevas "
          f"({len(X_nuevo) - len(keep)} duplicadas ya en la base).")
    if not keep:
        print("Nada nuevo que anexar: todas las ventanas ya estaban en la base.")
        return 0
    rng = random.Random(1234)
    rng.shuffle(keep)
    hueco = MAX_EJEMPLOS - len(y_viejo)
    if hueco <= 0:
        print(f"La base ya está en el tope ({MAX_EJEMPLOS} ejemplos): no se "
              f"puede anexar más sin crear el dataset desde cero.",
              file=sys.stderr)
        return 1
    if len(keep) > hueco:
        print(f"Aviso: solo caben {hueco} de {len(keep)} ventanas nuevas "
              f"(tope {MAX_EJEMPLOS} ejemplos). Se añaden las primeras "
              f"{hueco} barajadas.")
        keep = keep[:hueco]
    X = np.concatenate([X_viejo,
                        np.stack([X_nuevo[i] for i in keep]).astype(np.float32)])
    y = np.concatenate([y_viejo,
                        np.array([y_nuevo[i] for i in keep], dtype=np.int64)])
    # Barajado final de la base ampliada (mismo seed: reproducible).
    idx = list(range(len(y)))
    rng.shuffle(idx)
    X = X[idx]
    y = y[idx]
    manifiesto["archivos"] = manifiesto.get("archivos", []) + entradas
    manifiesto["debil"] = manifiesto.get("debil", 0) + debil
    manifiesto["ejemplos_por_clase"] = [0] * N_CLASES
    for c in y:
        manifiesto["ejemplos_por_clase"][int(c)] += 1
    manifiesto["total"] = int(len(y))
    manifiesto["actualizado"] = time.strftime("%Y-%m-%dT%H:%M:%S")
    return _guardar_dataset(salida, X, y, manifiesto)


# ---------------------------------------------------------------------------
# Modelo (pesos como dict de arrays numpy; mismo en torch y en numpy)
# ---------------------------------------------------------------------------
def inicializar_pesos(rng):
    s1 = math.sqrt(2.0 / N_FEATURES)
    s2 = math.sqrt(2.0 / 64)
    s3 = math.sqrt(2.0 / 32)
    return {
        "W1": (rng.standard_normal((N_FEATURES, 64)) * s1).astype(np.float32),
        "b1": np.zeros(64, dtype=np.float32),
        "W2": (rng.standard_normal((64, 32)) * s2).astype(np.float32),
        "b2": np.zeros(32, dtype=np.float32),
        "W3": (rng.standard_normal((32, N_CLASES)) * s3).astype(np.float32),
        "b3": np.zeros(N_CLASES, dtype=np.float32),
    }


def forward_np(pesos, X):
    h1 = np.maximum(X @ pesos["W1"] + pesos["b1"], 0)
    h2 = np.maximum(h1 @ pesos["W2"] + pesos["b2"], 0)
    logits = h2 @ pesos["W3"] + pesos["b3"]
    logits = logits - logits.max(axis=1, keepdims=True)
    e = np.exp(logits)
    return e / e.sum(axis=1, keepdims=True), h1, h2


def entrenar_numpy(Xtr, ytr, Xva, yva, epocas=300):
    rng = np.random.default_rng(7)
    pesos = inicializar_pesos(rng)
    # Adam, lote completo (el dataset es pequeño por diseño).
    m = {k: np.zeros_like(v) for k, v in pesos.items()}
    v = {k: np.zeros_like(v) for k, v in pesos.items()}
    lr, b1, b2, eps = 0.01, 0.9, 0.999, 1e-8
    mejor, sin_mejora, t = -1.0, 0, 0
    n = len(Xtr)
    for ep in range(1, epocas + 1):
        probs, h1, h2 = forward_np(pesos, Xtr)
        pred_va = forward_np(pesos, Xva)[0].argmax(axis=1)
        acc_va = float((pred_va == yva).mean())
        if acc_va > mejor + 1e-4:
            mejor, sin_mejora = acc_va, 0
            mejor_pesos = {k: v2.copy() for k, v2 in pesos.items()}
        else:
            sin_mejora += 1
            if sin_mejora >= 30:
                print(f"  Early stopping en época {ep} (val {acc_va:.3f}).")
                break
        # Gradientes (softmax + cross-entropy).
        dlog = probs.copy()
        dlog[np.arange(n), ytr] -= 1.0
        dlog /= n
        gW3 = h2.T @ dlog
        gb3 = dlog.sum(axis=0)
        dh2 = dlog @ pesos["W3"].T
        dh2[h2 <= 0] = 0
        gW2 = h1.T @ dh2
        gb2 = dh2.sum(axis=0)
        dh1 = dh2 @ pesos["W2"].T
        dh1[h1 <= 0] = 0
        gW1 = Xtr.T @ dh1
        gb1 = dh1.sum(axis=0)
        grads = {"W1": gW1, "b1": gb1, "W2": gW2, "b2": gb2, "W3": gW3, "b3": gb3}
        t += 1
        for k in pesos:
            m[k] = b1 * m[k] + (1 - b1) * grads[k]
            v[k] = b2 * v[k] + (1 - b2) * (grads[k] ** 2)
            mh = m[k] / (1 - b1 ** t)
            vh = v[k] / (1 - b2 ** t)
            pesos[k] -= lr * mh / (np.sqrt(vh) + eps)
        if ep % 50 == 0:
            print(f"  época {ep}: precisión validación {acc_va:.3f}")
    return mejor_pesos, mejor


def entrenar_torch(Xtr, ytr, Xva, yva, epocas=40):
    import torch
    import torch.nn as nn
    torch.manual_seed(7)
    modelo = nn.Sequential(nn.Linear(N_FEATURES, 64), nn.ReLU(),
                           nn.Linear(64, 32), nn.ReLU(),
                           nn.Linear(32, N_CLASES))
    opt = torch.optim.Adam(modelo.parameters(), lr=0.005)
    loss_fn = nn.CrossEntropyLoss()
    Xt = torch.tensor(Xtr)
    yt = torch.tensor(ytr)
    Xv = torch.tensor(Xva)
    yv = torch.tensor(yva)
    mejor, sin_mejora = -1.0, 0
    mejor_estado = None
    n = len(Xtr)
    for ep in range(1, epocas + 1):
        modelo.train()
        perm = torch.randperm(n)
        for i in range(0, n, 256):
            idx = perm[i:i + 256]
            opt.zero_grad()
            loss = loss_fn(modelo(Xt[idx]), yt[idx])
            loss.backward()
            opt.step()
        modelo.eval()
        with torch.no_grad():
            acc = float((modelo(Xv).argmax(1) == yv).float().mean())
        if acc > mejor + 1e-4:
            mejor, sin_mejora = acc, 0
            mejor_estado = {k: v.clone() for k, v in modelo.state_dict().items()}
        else:
            sin_mejora += 1
            if sin_mejora >= 6:
                print(f"  Early stopping en época {ep} (val {acc:.3f}).")
                break
        if ep % 10 == 0:
            print(f"  época {ep}: precisión validación {acc:.3f}")
    sd = mejor_estado
    return {
        "W1": sd["0.weight"].numpy().T.astype(np.float32),
        "b1": sd["0.bias"].numpy().astype(np.float32),
        "W2": sd["2.weight"].numpy().T.astype(np.float32),
        "b2": sd["2.bias"].numpy().astype(np.float32),
        "W3": sd["4.weight"].numpy().T.astype(np.float32),
        "b3": sd["4.bias"].numpy().astype(np.float32),
    }, mejor


def precision_regla(X, y):
    pred = np.array([clasificador_regla(f) for f in X])
    return float((pred == y).mean())


# ---------------------------------------------------------------------------
# Exportación ONNX (grafo real) + pesos.json + model_info.json
# ---------------------------------------------------------------------------
def exportar_onnx(pesos, ruta_onnx):
    try:
        import onnx
        from onnx import helper, TensorProto, numpy_helper
    except ImportError:
        return False, "paquete `onnx` no instalado (pip install onnx)"
    def tensor(nombre, arr):
        return numpy_helper.from_array(arr.astype(np.float32), name=nombre)
    nodos = [
        helper.make_node("Gemm", ["X", "W1", "b1"], ["h1_pre"], transB=0),
        helper.make_node("Relu", ["h1_pre"], ["h1"]),
        helper.make_node("Gemm", ["h1", "W2", "b2"], ["h2_pre"], transB=0),
        helper.make_node("Relu", ["h2_pre"], ["h2"]),
        helper.make_node("Gemm", ["h2", "W3", "b3"], ["logits"], transB=0),
        helper.make_node("Softmax", ["logits"], ["Y"], axis=1),
    ]
    grafo = helper.make_graph(
        nodos, "untrunc_onnx_mlp",
        [helper.make_tensor_value_info("X", TensorProto.FLOAT, [None, N_FEATURES])],
        [helper.make_tensor_value_info("Y", TensorProto.FLOAT, [None, N_CLASES])],
        [tensor("W1", pesos["W1"]), tensor("b1", pesos["b1"]),
         tensor("W2", pesos["W2"]), tensor("b2", pesos["b2"]),
         tensor("W3", pesos["W3"]), tensor("b3", pesos["b3"])])
    modelo = helper.make_model(grafo, producer_name="untrunc-entrenar",
                               opset_imports=[helper.make_opsetid("", 13)])
    modelo.ir_version = 8  # compatible con ONNX Runtime 1.17+
    onnx.checker.check_model(modelo)
    onnx.save(modelo, str(ruta_onnx))
    return True, ""


FORMAS_PESOS = {"W1": (N_FEATURES, 64), "b1": (64,), "W2": (64, 32),
                "b2": (32,), "W3": (32, N_CLASES), "b3": (N_CLASES,)}


def pesos_json_valido(ruta: Path) -> bool:
    """Comprueba que pesos.json se puede leer y tiene las formas que
    espera el programa (C++). Es lo que el programa carga: sin él el
    modelo no sirve aunque exista modelo.onnx."""
    try:
        pj = json.loads(ruta.read_text(encoding="utf-8"))
        for k, forma in FORMAS_PESOS.items():
            n = 1
            for d in forma:
                n *= d
            if len(pj[k]) != n:
                return False
        return True
    except Exception:  # noqa: BLE001
        return False


def guardar_modelo(pesos, salida: Path, info_extra):
    """Guarda el modelo de forma ATÓMICA: primero escribe los tres
    archivos con extensión .tmp, comprueba pesos.json, copia el modelo
    que hubiera a <salida>_anterior (copia de seguridad que el programa
    usa si el activo se estropea) y solo entonces reemplaza; el último
    en reemplazarse es model_info.json, que es el que marca el modelo
    como presente. Antes se escribía directamente encima: un corte a
    mitad (o un pesos.json perdido) dejaba un modelo activo inservible
    que el programa elegía igual y el ONNX dejaba de funcionar."""
    import shutil
    salida.mkdir(parents=True, exist_ok=True)
    tmp = {n: salida / (n + ".tmp") for n in ("pesos.json", "modelo.onnx", "model_info.json")}
    pesos_json = {k: [round(float(x), 6) for x in v.flatten()]
                  for k, v in pesos.items()}
    pesos_json["_formas"] = {k: list(v.shape) for k, v in pesos.items()}
    tmp["pesos.json"].write_text(json.dumps(pesos_json), encoding="utf-8")
    ok, err = exportar_onnx(pesos, tmp["modelo.onnx"])
    info = {"nombre": "untrunc_onnx_mlp", "arquitectura": ARQUITECTURA,
            "parametros": PARAMETROS, "ventana": VENTANA,
            "features": N_FEATURES, "clases": CLASES,
            "tamano_bytes": 0, "onnx_exportado": ok,
            "onnx_error": err, "cuantizado": "fp32 (sin cuantizar en el prototipo)",
            "fecha": time.strftime("%Y-%m-%dT%H:%M:%S")}
    info.update(info_extra)
    if ok:
        info["tamano_bytes"] = tmp["modelo.onnx"].stat().st_size
    tmp["model_info.json"].write_text(
        json.dumps(info, indent=2, ensure_ascii=False), encoding="utf-8")
    if not pesos_json_valido(tmp["pesos.json"]):
        for t in tmp.values():
            if t.exists():
                t.unlink()
        raise RuntimeError(f"no se pudo escribir pesos.json en {salida}")
    # Copia de seguridad del modelo anterior (si era utilizable).
    if (salida / "model_info.json").exists() and pesos_json_valido(salida / "pesos.json"):
        respaldo = salida.parent / (salida.name + "_anterior")
        respaldo.mkdir(parents=True, exist_ok=True)
        for n in ("pesos.json", "modelo.onnx", "model_info.json"):
            if (salida / n).exists():
                shutil.copy2(salida / n, respaldo / n)
    if not ok and (salida / "modelo.onnx").exists():
        (salida / "modelo.onnx").unlink()  # el viejo no corresponde a estos pesos
    for n in ("pesos.json", "modelo.onnx", "model_info.json"):
        if tmp[n].exists():
            os.replace(tmp[n], salida / n)
    return ok, err, info


def entrenar(dataset_dir: Path, salida: Path, motor="auto"):
    npz = np.load(dataset_dir / "dataset.npz")
    X, y = npz["X"], npz["y"]
    rng = np.random.default_rng(7)
    idx = rng.permutation(len(y))
    nva = max(20, int(0.2 * len(y)))
    va, tr = idx[:nva], idx[nva:]
    Xtr, ytr, Xva, yva = X[tr], y[tr], X[va], y[va]
    base = precision_regla(Xva, yva)
    print(f"Ejemplos: {len(y)} (entrenamiento {len(tr)}, validación {len(va)}).")
    print(f"Línea base de la heurística en validación: {base:.3f}")
    uso_torch = False
    if motor in ("auto", "torch"):
        try:
            import torch  # noqa: F401
            uso_torch = True
        except Exception as e:  # noqa: BLE001 - ImportError u otro fallo al importar
            # Puntos 2/3 (2026-11-07): torch puede romperse también al
            # IMPORTARSE (no solo al entrenar, en Adam); en "auto" eso
            # también cae a NumPy en vez de morir con rc=1.
            if motor == "torch":
                if isinstance(e, ImportError):
                    print("ERROR: PyTorch no está instalado. "
                          "Instálalo con:\n"
                          "  pip install torch --index-url https://download.pytorch.org/whl/cpu",
                          file=sys.stderr)
                else:
                    print(f"ERROR: PyTorch no se pudo usar ({type(e).__name__}: {e}). "
                          "Instálalo con:\n"
                          "  pip install torch --index-url https://download.pytorch.org/whl/cpu",
                          file=sys.stderr)
                return 3
            # Sin PyTorch es lo normal (el ZIP trae Python + numpy):
            # no se sugiere instalar nada. Solo se avisa si PyTorch
            # está instalado pero roto.
            if not isinstance(e, ImportError):
                print(f"PyTorch está instalado pero no se pudo usar "
                      f"({type(e).__name__}: {e}); se entrena con NumPy "
                      "(mismo modelo, más lento).")
    if uso_torch:
        print("Motor: PyTorch (CPU).")
        # Reparaciones (2026-11-07, PC de Angel): PyTorch puede
        # importarse bien y romperse DESPUÉS, al arrancar Adam
        # (torch._dynamo -> torch.onnx -> import transformers ->
        # AttributeError en torch.utils._pytree por incompatibilidad
        # torch/transformers). El fallback antiguo solo saltaba si
        # `import torch` fallaba, así que ese caso moría con rc=1
        # sin entrenar nada. Ahora CUALQUIER fallo de PyTorch cae
        # a NumPy con un aviso claro en español; el entrenamiento
        # sigue y la puerta de calidad decide igual.
        try:
            pesos, acc = entrenar_torch(Xtr, ytr, Xva, yva)
        except Exception as e:  # noqa: BLE001 - cualquier rotura de torch
            print(f"PyTorch falló durante el entrenamiento "
                  f"({type(e).__name__}: {e}). Se entrena con NumPy "
                  f"en su lugar (mismo modelo, más lento). Si quieres "
                  f"usar PyTorch, revisa que torch y transformers sean "
                  f"compatibles en tu Python.")
            uso_torch = False
            pesos, acc = entrenar_numpy(Xtr, ytr, Xva, yva)
    else:
        print("Motor: NumPy.")
        pesos, acc = entrenar_numpy(Xtr, ytr, Xva, yva)
    print(f"Precisión en validación (modelo): {acc:.3f}  |  heurística: {base:.3f}")
    # Puerta de calidad: si el modelo NO supera a la heurística, NO se
    # reemplaza el modelo activo; se guarda como candidato y se avisa.
    if acc < 0.70 or acc < base + 0.05:
        cand = salida / "modelo_candidato"
        guardar_modelo(pesos, cand, {"ejemplos": int(len(y)),
                                     "precision_validacion": round(acc, 4),
                                     "precision_baseline": round(base, 4),
                                     "aceptado": False})
        print(f"El modelo NO supera a la heurística (o está por debajo del "
              f"70%). NO se ha reemplazado el modelo activo. Quedó como "
              f"candidato en {cand} para revisarlo.")
        problemas = verificar_privacidad(dataset_dir) + verificar_privacidad(salida)
        if problemas:
            print("ERROR DE PRIVACIDAD:", file=sys.stderr)
            for p in problemas:
                print(f"  - {p}", file=sys.stderr)
            return 5
        return 4
    ok, err, info = guardar_modelo(
        pesos, salida, {"ejemplos": int(len(y)),
                        "precision_validacion": round(acc, 4),
                        "precision_baseline": round(base, 4),
                        "aceptado": True, "motor": "torch" if uso_torch else "numpy"})
    if ok:
        print(f"Modelo aceptado y guardado en {salida} "
              f"({info['tamano_bytes']} bytes .onnx, {PARAMETROS} parámetros).")
    else:
        print(f"Modelo aceptado y guardado en {salida} ({PARAMETROS} parámetros).")
    # Sin el paquete `onnx` (opcional) no se exporta el .onnx, pero el
    # programa usa pesos.json: no es un problema y no se avisa. Solo
    # se avisa si `onnx` está instalado y la exportación falló.
    if not ok and "no instalado" not in err:
        print(f"Aviso: el .onnx no se exportó ({err}); pesos.json y "
              f"model_info.json sí se guardaron y el modelo se puede usar en C++.")
    problemas = verificar_privacidad(dataset_dir) + verificar_privacidad(salida)
    if problemas:
        print("ERROR DE PRIVACIDAD: el entrenamiento no es seguro, "
              "no se usará este modelo:", file=sys.stderr)
        for p in problemas:
            print(f"  - {p}", file=sys.stderr)
        return 5
    print("Privacidad verificada: ni en el dataset ni en el modelo hay "
          "ningún vídeo ni rutas completas.")
    return 0


def probar(modelo_dir: Path, dataset_dir: Path):
    info_p = modelo_dir / "model_info.json"
    pesos_p = modelo_dir / "pesos.json"
    if not info_p.exists() or not pesos_p.exists():
        print(f"ERROR: no hay un modelo válido en {modelo_dir} "
              f"(faltan model_info.json / pesos.json).", file=sys.stderr)
        return 1
    info = json.loads(info_p.read_text(encoding="utf-8"))
    pj = json.loads(pesos_p.read_text(encoding="utf-8"))
    formas = pj["_formas"]
    pesos = {k: np.array(pj[k], dtype=np.float32).reshape(formas[k])
             for k in ("W1", "b1", "W2", "b2", "W3", "b3")}
    npz = np.load(dataset_dir / "dataset.npz")
    X, y = npz["X"], npz["y"]
    probs, _, _ = forward_np(pesos, X)
    pred = probs.argmax(axis=1)
    acc = float((pred == y).mean())
    base = precision_regla(X, y)
    print(f"Modelo: {info.get('nombre')} | ejemplos de entrenamiento: "
          f"{info.get('ejemplos')} | precisión en entrenamiento: "
          f"{info.get('precision_validacion')}")
    print(f"Precisión sobre este dataset ({len(y)} ejemplos): {acc:.3f} "
          f"| heurística: {base:.3f}")
    for c, nombre in enumerate(CLASES):
        sel = y == c
        if sel.sum():
            print(f"  {nombre}: {float((pred[sel] == c).mean()):.3f} "
                  f"({int(sel.sum())} ejemplos)")
    return 0 if acc >= base else 4


def resumen_base(base: Path, dataset_dir: Path = None, modelo_dir: Path = None):
    """Resumen de la BASE PERSISTENTE (§12): lo que queda guardado
    sin necesidad de volver a cargar ningún vídeo. Lee solo
    dataset_manifest.json + model_info.json (nunca vídeos).
    Devuelve 0 si hay base, 2 si todavía no hay ninguna."""
    ds_dir = dataset_dir or base / "dataset"
    modelo_dir = modelo_dir or base / "modelo_activo"
    X, y, manifiesto = _cargar_dataset_existente(ds_dir)
    if X is None:
        print("Sin base todavía: agrega vídeos buenos y pulsa "
              "«Entrenar modelo». No hay dataset guardado.")
        return 2
    total = manifiesto.get("total", len(y))
    por_clase = manifiesto.get("ejemplos_por_clase", [0] * N_CLASES)
    print(f"Base de entrenamiento guardada: {total} ejemplos")
    for c, nombre in enumerate(CLASES):
        print(f"  {nombre}: {por_clase[c] if c < len(por_clase) else 0}")
    print(f"Actualizada: {manifiesto.get('actualizado', manifiesto.get('fecha', '?'))}")
    print("Vídeos que aportaron (solo nombre base + hash, nunca ruta):")
    for ent in manifiesto.get("archivos", []):
        if ent.get("ejemplos"):
            print(f"  {ent.get('nombre')} ({ent.get('hash', '?')}) — "
                  f"{ent.get('ejemplos')} ejemplos — {ent.get('tipo')} — "
                  f"{ent.get('fecha', '')}")
    info_p = modelo_dir / "model_info.json"
    if info_p.exists():
        try:
            info = json.loads(info_p.read_text(encoding="utf-8"))
            print(f"Modelo activo: {info.get('nombre')} · "
                  f"{info.get('tamano_bytes', 0)} bytes · "
                  f"precisión en validación {info.get('precision_validacion')} "
                  f"(heurística: {info.get('precision_baseline')}) · "
                  f"{info.get('fecha', '')}")
        except Exception:
            print("Modelo activo: existe pero su model_info.json no se pudo leer.")
    else:
        print("Modelo activo: ninguno todavía (usa el modelo base incluido).")
    print("Para añadir más vídeos sin recargar los anteriores: "
          "anexar-dataset o entrenar --anexar.")
    return 0


def borrar_base(base: Path, dataset_dir: Path, modelo_dir: Path,
                base_modelo_dir: Path):
    """Borra la base de entrenamiento (dataset + modelo entrenado)
    y vuelve al modelo base mínimo incluido. NUNCA borra los
    vídeos originales del usuario: no están en estas carpetas
    (ver §11) y aquí solo se eliminan los nombres de archivo
    permitidos. Exige confirmación explícita del llamador."""
    for carpeta in (dataset_dir, modelo_dir):
        carpeta.mkdir(parents=True, exist_ok=True)
        for nombre in list(os.listdir(carpeta)):
            if nombre in ARCHIVOS_PERMITIDOS:
                (carpeta / nombre).unlink()
            else:
                print(f"Aviso: no se borra el archivo no esperado "
                      f"{nombre} (se deja y lo señalará "
                      f"verificar-privacidad).", file=sys.stderr)
    # Vuelve al modelo base mínimo incluido.
    if base_modelo_dir and (base_modelo_dir / "model_info.json").exists():
        import shutil
        for nombre in ("modelo.onnx", "pesos.json", "model_info.json"):
            src = base_modelo_dir / nombre
            if src.exists():
                shutil.copy2(src, modelo_dir / nombre)
        print(f"Base borrada. Modelo base restablecido en {modelo_dir}. "
              f"Tus vídeos originales no se tocaron: nunca estuvieron aquí.")
    else:
        print("Base borrada. No se encontró el modelo base incluido para "
              "restablecerlo; restablécelo desde la pestaña Entrenar.")
    return 0


# ---------------------------------------------------------------------------
# Modelo de resincronizacion (ver src/resinc_ia.h)
# ---------------------------------------------------------------------------
# Decide SOLO los candidatos tras una zona danada en los que la cadena no
# decide (0 o 1 fotogramas siguientes encajan). Entra un TSV de numeros que
# genera `untrunc --dataset-resinc` (sin rutas ni bytes de video). Sale
# resinc_modelo.txt: regresion logistica sobre las caracteristicas, con un
# umbral elegido para no aceptar ningun falso en entrenamiento.
#
# Validacion honesta: cada video se deja fuera una vez; con los demas se
# entrena y se elige el umbral, y se cuenta cuantos reales y falsos del
# video no visto pasarian. El modelo queda "activo 1" (el programa lo usa)
# solo si en esa validacion hubo 0 falsos aceptados y al menos
# RESINC_MIN_REALES reales recuperados; si no, se guarda con "activo 0"
# y el programa no lo usa.

RESINC_NFEAT = 17
RESINC_MIN_REALES = 3


def _leer_tsv_resinc(ruta: Path):
    lineas = ruta.read_text(encoding="utf-8").splitlines()
    if not lineas:
        raise ValueError("el TSV está vacío")
    cab = lineas[0].split("\t")
    if cab[:6] != ["video", "tipo", "semilla", "etiqueta", "cadena", "codec"] or \
            len(cab) != 6 + RESINC_NFEAT:
        raise ValueError("el TSV no es de `untrunc --dataset-resinc` (o es de otra versión)")
    video, et, cad, X = [], [], [], []
    for l in lineas[1:]:
        c = l.split("\t")
        if len(c) != len(cab):
            continue
        video.append(int(c[0]))
        et.append(int(c[3]))
        cad.append(int(c[4]))
        X.append([float(x) for x in c[6:]])
    return (cab[6:], np.array(video), np.array(et), np.array(cad),
            np.array(X, dtype=np.float64).reshape(-1, RESINC_NFEAT))


def _logistica(X, y, l2=1e-2, epocas=3000, lr=0.5):
    """Regresion logistica con clases equilibradas (descenso de gradiente)."""
    media = X.mean(axis=0)
    escala = X.std(axis=0)
    escala[escala < 1e-9] = 1.0
    Z = (X - media) / escala
    n, d = Z.shape
    pos = max(1, int(y.sum()))
    neg = max(1, n - pos)
    peso = np.where(y == 1, n / (2.0 * pos), n / (2.0 * neg))
    w = np.zeros(d)
    b = 0.0
    for _ in range(epocas):
        p = 1.0 / (1.0 + np.exp(-(Z @ w + b)))
        g = (p - y) * peso
        w -= lr * (Z.T @ g / n + l2 * w)
        b -= lr * g.mean()
    return media, escala, w, b


def _red(X, y, ocultas=8, l2=1e-3, epocas=4000, lr=0.01, semilla=0):
    """Red 17-ocultas-1 (ReLU, sigmoide), clases equilibradas, Adam."""
    media = X.mean(axis=0)
    escala = X.std(axis=0)
    escala[escala < 1e-9] = 1.0
    Z = (X - media) / escala
    n, d = Z.shape
    rng = np.random.default_rng(semilla)
    W1 = rng.normal(0, 1.0 / math.sqrt(d), (ocultas, d))
    b1 = np.zeros(ocultas)
    W2 = rng.normal(0, 1.0 / math.sqrt(ocultas), (1, ocultas))
    b2 = np.zeros(1)
    pos = max(1, int(y.sum()))
    neg = max(1, n - pos)
    peso = np.where(y == 1, n / (2.0 * pos), n / (2.0 * neg))
    params = [W1, b1, W2, b2]
    m1 = [np.zeros_like(q) for q in params]
    m2 = [np.zeros_like(q) for q in params]
    for t in range(1, epocas + 1):
        h = np.maximum(0, Z @ W1.T + b1)
        p = 1.0 / (1.0 + np.exp(-(h @ W2.T + b2)[:, 0]))
        g = ((p - y) * peso / n)[:, None]
        gW2 = g.T @ h + l2 * W2
        gb2 = g.sum(axis=0)
        gh = (g @ W2) * (h > 0)
        gW1 = gh.T @ Z + l2 * W1
        gb1 = gh.sum(axis=0)
        for i, gr in enumerate((gW1, gb1, gW2, gb2)):
            m1[i] = 0.9 * m1[i] + 0.1 * gr
            m2[i] = 0.999 * m2[i] + 0.001 * gr * gr
            params[i] -= lr * (m1[i] / (1 - 0.9 ** t)) / (np.sqrt(m2[i] / (1 - 0.999 ** t)) + 1e-8)
    return ("red", media, escala, W1, b1, W2, b2)


def _puntuar(modelo, X):
    if isinstance(modelo[0], str):
        _, media, escala, W1, b1, W2, b2 = modelo
        h = np.maximum(0, ((X - media) / escala) @ W1.T + b1)
        return 1.0 / (1.0 + np.exp(-(h @ W2.T + b2)[:, 0]))
    media, escala, w, b = modelo
    return 1.0 / (1.0 + np.exp(-(((X - media) / escala) @ w + b)))


def _umbral_sin_falsos(p, y):
    """El menor umbral que no deja pasar ningun negativo (con margen)."""
    neg = p[y == 0]
    tope = float(neg.max()) if len(neg) else 0.5
    return min(0.999999, max(0.5, tope + 0.25 * (1.0 - tope)))


RESINC_MAX_PLIEGUES = 5       # validacion por grupos (camaras): como mucho 5 pliegues
RESINC_MAX_NEGATIVOS = 20000  # submuestreo de falsos para que entrenar no tarde horas


def _leer_validacion(ruta: Path):
    """(activo, reales_ok, reales_tot, falsos) de un modelo guardado, o None."""
    try:
        d = {}
        for l in ruta.read_text(encoding="utf-8").splitlines():
            c = l.split()
            if len(c) == 2 and c[0] in ("activo", "val_reales", "val_total", "val_falsos"):
                d[c[0]] = int(c[1])
        return (d.get("activo", 0) == 1, d.get("val_reales", 0), d.get("val_total", 0),
                d.get("val_falsos", 0))
    except OSError:
        return None


def entrenar_resinc(tsv: Path, salida: Path, tipo="red", solo_si_mejora=False):
    try:
        nombres, video, et, cad, X = _leer_tsv_resinc(tsv)
    except (OSError, ValueError) as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 2
    m = cad < 2
    video, et, X = video[m], et[m], X[m]
    neg = np.flatnonzero(et == 0)
    if len(neg) > RESINC_MAX_NEGATIVOS:
        rng = np.random.default_rng(0)
        quitar = rng.choice(neg, len(neg) - RESINC_MAX_NEGATIVOS, replace=False)
        dejar = np.ones(len(et), bool)
        dejar[quitar] = False
        video, et, X = video[dejar], et[dejar], X[dejar]
        print(f"(se usan {RESINC_MAX_NEGATIVOS} falsos elegidos al azar)")
    videos = sorted(set(video.tolist()))
    print(f"Candidatos sin cadena: {len(et)} ({int(et.sum())} reales) de "
          f"{len(videos)} vídeos.")
    if et.sum() < 2 or (et == 0).sum() < 2 or len(videos) < 2:
        print("ERROR: hacen falta reales y falsos de al menos 2 vídeos.", file=sys.stderr)
        return 2

    # Validacion dejando fuera cada grupo (video o camara): con muchos
    # grupos se reparten en RESINC_MAX_PLIEGUES pliegues.
    pliegue = {v: i % RESINC_MAX_PLIEGUES for i, v in enumerate(videos)}
    grupo = np.array([pliegue[v] for v in video.tolist()])
    reales_ok = falsos_ok = reales_tot = 0
    por_video = []
    for v in sorted(set(pliegue.values())):
        tr, va = grupo != v, grupo == v
        if et[tr].sum() == 0:
            continue
        mod = _red(X[tr], et[tr]) if tipo == "red" else _logistica(X[tr], et[tr])
        umbral = _umbral_sin_falsos(_puntuar(mod, X[tr]), et[tr])
        pv = _puntuar(mod, X[va])
        r = int(((pv >= umbral) & (et[va] == 1)).sum())
        f = int(((pv >= umbral) & (et[va] == 0)).sum())
        reales_ok += r
        falsos_ok += f
        reales_tot += int(et[va].sum())
        por_video.append((v, int(et[va].sum()), r, f))
    activo = falsos_ok == 0 and reales_ok >= RESINC_MIN_REALES
    print(f"Validación (cada vídeo sin verlo en el entrenamiento): {reales_ok} de "
          f"{reales_tot} reales recuperados, {falsos_ok} falsos aceptados.")
    for v, tot, r, f in por_video:
        if tot or f:
            print(f"  vídeo {v}: {r}/{tot} reales, {f} falsos")

    mod = _red(X, et) if tipo == "red" else _logistica(X, et)
    umbral = _umbral_sin_falsos(_puntuar(mod, X), et)
    salida.parent.mkdir(parents=True, exist_ok=True)
    fmt = lambda v: " ".join(f"{x:.9g}" for x in v)
    texto = [
        "untrunc-resinc 1",
        "# Modelo de resincronizacion (train_onnx.py entrenar-resinc).",
        f"# Validacion dejando fuera cada video: {reales_ok}/{reales_tot} reales, "
        f"{falsos_ok} falsos aceptados; {len(videos)} videos, {len(et)} candidatos.",
        "# caracteristicas: " + " ".join(nombres),
        f"activo {1 if activo else 0}",
        f"val_reales {reales_ok}",
        f"val_total {reales_tot}",
        f"val_falsos {falsos_ok}",
        f"umbral {umbral:.9g}",
        f"nfeat {RESINC_NFEAT}",
    ]
    if isinstance(mod[0], str):
        _, media, escala, W1, b1, W2, b2 = mod
        texto += ["media " + fmt(media), "escala " + fmt(escala),
                  f"capa {RESINC_NFEAT} {len(b1)}"] + [fmt(fila) for fila in W1] + [fmt(b1),
                  f"capa {len(b1)} 1", fmt(W2[0]), fmt(b2)]
    else:
        media, escala, w, b = mod
        texto += ["media " + fmt(media), "escala " + fmt(escala),
                  f"capa {RESINC_NFEAT} 1", fmt(w), f"{b:.9g}"]
    if solo_si_mejora and salida.exists():
        ant = _leer_validacion(salida)
        if ant and ant[0]:
            mejora = activo and ant[3] == 0 and (
                reales_ok / max(1, reales_tot) >= ant[1] / max(1, ant[2]))
            if not mejora:
                print(f"SIN CAMBIOS: el modelo activo ({ant[1]}/{ant[2]} reales) no se sustituye "
                      f"por uno que no lo mejora ({reales_ok}/{reales_tot}, {falsos_ok} falsos).")
                return 0
    tmp = salida.with_suffix(".tmp")
    tmp.write_text("\n".join(texto) + "\n", encoding="utf-8")
    os.replace(tmp, salida)
    print(f"Modelo guardado en {salida} (umbral {umbral:.4f}).")
    if activo:
        print("ACTIVO: en vídeos no vistos no aceptó ningún falso; el motor lo usará.")
    else:
        motivo = (f"aceptó {falsos_ok} falsos" if falsos_ok
                  else f"solo recuperó {reales_ok} reales (mínimo {RESINC_MIN_REALES})")
        print(f"NO ACTIVO: en la validación {motivo}; el programa no lo usará.")
    return 0


def dir_entrenamiento():
    return Path.home() / ".config" / "untrunc" / "entrenamiento"


def main():
    ap = argparse.ArgumentParser(prog="train_onnx.py",
                                 description="Entrenamiento local del motor ONNX pequeño de Untrunc.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("crear-dataset", help="Crea dataset.npz desde vídeos buenos/corruptos.")
    c.add_argument("--buenos", nargs="*", default=[])
    c.add_argument("--corruptos", nargs="*", default=[])
    c.add_argument("--salida", required=True)
    c.add_argument("--max-por-archivo", type=int, default=MAX_POR_ARCHIVO)
    e = sub.add_parser("entrenar", help="Entrena el MLP y exporta modelo.onnx.")
    e.add_argument("--dataset", default=None)
    e.add_argument("--salida", default=None)
    e.add_argument("--buenos", nargs="*", default=[],
                   help="Vídeos buenos (rutas directas, sin archivo de "
                        "listas: es el camino que usa la GUI, que no "
                        "persiste listas en disco).")
    e.add_argument("--corruptos", nargs="*", default=[],
                   help="Vídeos corruptos (rutas directas, sin archivo "
                        "de listas).")
    e.add_argument("--listas", default=None,
                   help="LEGADO (CLI): carpeta con buenos.txt/corruptos.txt. "
                        "Se lee solo en memoria y nunca se copia. La GUI ya "
                        "no escribe estos archivos.")
    e.add_argument("--anexar", action="store_true",
                   help="§12: con --buenos/--corruptos, ANEXA los vídeos "
                        "nuevos a la base guardada (anexar_dataset) en vez "
                        "de crear el dataset desde cero; luego re-entrena "
                        "desde cero sobre la base ampliada (ver entrenar: "
                        "es más estable que el fine-tuning para un MLP de "
                        "19.172 parámetros y 12 s de entrenamiento). Sin "
                        "--buenos/--corruptos, entrena sobre la base que "
                        "ya hay guardada, sin pedir vídeos.")
    e.add_argument("--motor", choices=["auto", "torch", "numpy"], default="auto")
    a = sub.add_parser("anexar-dataset",
                       help="§12: añade vídeos nuevos al dataset guardado "
                            "sin borrarlo ni recargar los anteriores.")
    a.add_argument("--buenos", nargs="*", default=[])
    a.add_argument("--corruptos", nargs="*", default=[])
    a.add_argument("--salida", required=True)
    a.add_argument("--max-por-archivo", type=int, default=MAX_POR_ARCHIVO)
    s = sub.add_parser("resumen",
                       help="§12: muestra la base de entrenamiento guardada "
                            "(ejemplos, por clase, vídeos que aportaron por "
                            "nombre base + hash, modelo activo) sin cargar "
                            "ningún vídeo.")
    s.add_argument("--carpeta", default=None,
                   help="Carpeta de entrenamiento (por defecto, la estándar).")
    s.add_argument("--dataset", default=None)
    s.add_argument("--modelo", default=None)
    b = sub.add_parser("borrar-base",
                       help="§12: borra el dataset y el modelo entrenado y "
                            "vuelve al modelo base mínimo. Nunca borra los "
                            "vídeos originales. Exige --confirmar.")
    b.add_argument("--base", default=None,
                   help="Carpeta del modelo base incluido.")
    b.add_argument("--destino", default=None,
                   help="Carpeta del modelo activo (por defecto, la estándar).")
    b.add_argument("--dataset", default=None,
                   help="Carpeta del dataset (por defecto, la estándar).")
    b.add_argument("--confirmar", action="store_true",
                   help="Obligatorio: confirma que se borra la base guardada.")
    v = sub.add_parser("verificar-privacidad",
                       help="Verifica que en una carpeta de entrenamiento no "
                            "haya ningún vídeo ni rutas completas (código 5 "
                            "si encuentra algo).")
    v.add_argument("--carpeta", default=None,
                   help="Carpeta a verificar (por defecto, la de entrenamiento).")
    p = sub.add_parser("probar", help="Evalúa un modelo sobre un dataset.")
    p.add_argument("--modelo", default=None)
    p.add_argument("--dataset", default=None)
    rs = sub.add_parser("entrenar-resinc",
                        help="Entrena el modelo de resincronización con el TSV de "
                             "`untrunc --dataset-resinc` y lo valida por vídeo.")
    rs.add_argument("--dataset", required=True, help="TSV de untrunc --dataset-resinc.")
    rs.add_argument("--tipo", choices=["red", "lineal"], default="red",
                    help="red: 17-8-1 (por defecto); lineal: regresión logística.")
    rs.add_argument("--solo-si-mejora", action="store_true",
                    help="No sustituir un modelo activo por uno que no lo mejore.")
    rs.add_argument("--salida", default=None,
                    help="Por defecto, resinc_modelo.txt en la carpeta de entrenamiento.")
    r = sub.add_parser("restablecer", help="Copia el modelo base a la carpeta del modelo activo.")
    r.add_argument("--base", required=True)
    r.add_argument("--destino", required=True)
    args = ap.parse_args()

    if args.cmd == "crear-dataset":
        if not args.buenos and not args.corruptos:
            print("ERROR: añade al menos un vídeo bueno con --buenos.", file=sys.stderr)
            return 2
        return crear_dataset(args.buenos, args.corruptos, Path(args.salida),
                             args.max_por_archivo)
    if args.cmd == "anexar-dataset":
        if not args.buenos and not args.corruptos:
            print("ERROR: añade al menos un vídeo nuevo con --buenos o "
                  "--corruptos.", file=sys.stderr)
            return 2
        return anexar_dataset(args.buenos, args.corruptos, Path(args.salida),
                              args.max_por_archivo)
    if args.cmd == "resumen":
        base = Path(args.carpeta) if args.carpeta else dir_entrenamiento()
        ds_dir = Path(args.dataset) if args.dataset else None
        mod_dir = Path(args.modelo) if args.modelo else None
        return resumen_base(base, ds_dir, mod_dir)
    if args.cmd == "borrar-base":
        if not args.confirmar:
            print("ERROR: borrar la base es destructivo: vuelve a llamar "
                  "con --confirmar. Esto borra el dataset guardado y el "
                  "modelo entrenado (los vídeos originales NO se tocan: "
                  "nunca estuvieron en la carpeta de entrenamiento).",
                  file=sys.stderr)
            return 2
        base = dir_entrenamiento()
        ds_dir = Path(args.dataset) if args.dataset else base / "dataset"
        out_dir = Path(args.destino) if args.destino else base / "modelo_activo"
        base_dir = Path(args.base) if args.base else None
        return borrar_base(base, ds_dir, out_dir, base_dir)
    if args.cmd == "verificar-privacidad":
        carpeta = Path(args.carpeta) if args.carpeta else dir_entrenamiento()
        problemas = verificar_privacidad(carpeta)
        if problemas:
            print(f"PRIVACIDAD: {len(problemas)} problema(s) en {carpeta}:",
                  file=sys.stderr)
            for p in problemas:
                print(f"  - {p}", file=sys.stderr)
            return 5
        print(f"Privacidad verificada: en {carpeta} no hay ningún vídeo, "
              f"ni rutas completas en el manifiesto, ni ventanas mayores "
              f"de 256 bytes en el dataset.")
        return 0
    if args.cmd == "entrenar":
        base = dir_entrenamiento()
        ds_dir = Path(args.dataset) if args.dataset else base / "dataset"
        out_dir = Path(args.salida) if args.salida else base / "modelo_activo"
        if args.buenos or args.corruptos:
            # §12: --anexar añade a la base guardada sin borrarla;
            # sin --anexar se crea el dataset desde cero (y la GUI
            # pide confirmación antes, porque borra la base anterior).
            # En ambos casos se RE-ENTRENA DESDE CERO sobre el
            # dataset resultante: con un MLP de 19.172 parámetros y
            # ~12 s por 4.500 ejemplos, entrenar desde cero es más
            # rápido y más estable que el fine-tuning desde los
            # pesos viejos (que puede olvidar lo aprendido de la
            # cámara anterior: olvido catastrófico). La puerta de
            # calidad (§5) sigue decidiendo si el modelo nuevo
            # reemplaza al activo.
            if args.anexar:
                rc = anexar_dataset(args.buenos, args.corruptos, ds_dir)
            else:
                rc = crear_dataset(args.buenos, args.corruptos, ds_dir)
            if rc:
                return rc
        elif args.anexar and not (ds_dir / "dataset.npz").exists():
            print("ERROR: --anexar sin vídeos nuevos necesita una base "
                  "guardada y no hay ninguna. Crea primero el dataset "
                  "con --buenos.", file=sys.stderr)
            return 2
        elif args.listas:
            listas = Path(args.listas)
            buenos, corruptos = [], []
            for nombre, dest in (("buenos.txt", buenos), ("corruptos.txt", corruptos)):
                f = listas / nombre
                if f.exists():
                    dest.extend(l.strip() for l in f.read_text(encoding="utf-8").splitlines()
                                if l.strip())
            if not buenos and not corruptos:
                print(f"ERROR: no hay listas en {listas} "
                      f"(se esperaban buenos.txt / corruptos.txt).", file=sys.stderr)
                return 2
            rc = crear_dataset(buenos, corruptos, ds_dir)
            if rc:
                return rc
        if not (ds_dir / "dataset.npz").exists():
            print(f"ERROR: no hay dataset en {ds_dir}. Primero crea uno con "
                  f"crear-dataset (o entrena con --buenos/--corruptos).", file=sys.stderr)
            return 2
        return entrenar(ds_dir, out_dir, args.motor)
    if args.cmd == "probar":
        base = dir_entrenamiento()
        modelo = Path(args.modelo) if args.modelo else base / "modelo_activo"
        ds_dir = Path(args.dataset) if args.dataset else base / "dataset"
        if not (ds_dir / "dataset.npz").exists():
            print(f"ERROR: no hay dataset en {ds_dir}.", file=sys.stderr)
            return 2
        return probar(modelo, ds_dir)
    if args.cmd == "entrenar-resinc":
        salida = Path(args.salida) if args.salida else dir_entrenamiento() / "resinc_modelo.txt"
        return entrenar_resinc(Path(args.dataset), salida, args.tipo, args.solo_si_mejora)
    if args.cmd == "restablecer":
        origen, destino = Path(args.base), Path(args.destino)
        if not (origen / "model_info.json").exists():
            print(f"ERROR: no hay modelo base en {origen}.", file=sys.stderr)
            return 2
        destino.mkdir(parents=True, exist_ok=True)
        import shutil
        for nombre in ("modelo.onnx", "pesos.json", "model_info.json"):
            src = origen / nombre
            if src.exists():
                shutil.copy2(src, destino / nombre)
        print(f"Modelo base restablecido en {destino} (se vuelve al modelo mínimo incluido).")
        return 0
    return 2


if __name__ == "__main__":
    sys.exit(main())
