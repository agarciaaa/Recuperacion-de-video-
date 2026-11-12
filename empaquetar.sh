#!/usr/bin/env bash
# empaquetar.sh — Untrunc-mejorado, Fase 3.2: ZIP portable (Windows) y ZIP de prueba (Linux)
#
# Uso:
#   ./empaquetar.sh linux [--salida DIR]     # ZIP de prueba para Linux (este entorno)
#   ./empaquetar.sh windows [--salida DIR]   # ZIP para Windows (en MSYS2 MINGW64)
#
# Qué hace: arma la carpeta dist/<paquete>/ con los ejecutables ya
# compilados + datos + documentación, verifica el contenido (que no
# falte nada, que no entre ningún vídeo ni dataset, que COPYING no
# se haya modificado) y comprime el ZIP.
#
# La compilación NO se hace aquí: antes hay que compilar con
#   make IS_RELEASE=1            (CLI: untrunc / untrunc.exe)
#   make untrunc-gui LIBUI_STATIC=1 IS_RELEASE=1   (GUI, solo Windows: untrunc-gui.exe)
# o con el target del Makefile:  make dist-linux  /  make dist-windows
#
# Decisión de diseño (por qué un script bash y no un instalador):
# - El proyecto es portable por diseño (el ZIP oficial ya era un ZIP,
#   no un instalador .msi/.exe): no hay instalador que mantener y el
#   usuario mueve la carpeta donde quiera, incluso a un USB.
# - Bash es el mismo shell que ya usan el Makefile y el CI de GitHub
#   Actions (.github/workflows/release.yml, MSYS2), así que el script
#   funciona igual en este Linux y en MSYS2 en Windows, sin depender
#   de PowerShell ni de herramientas de instalador.
# - El Makefile solo expone los targets dist-linux / dist-windows,
#   que llaman a este script: una sola fuente de verdad para la
#   lista de archivos del paquete.
#
# Sin firma de código: el ZIP NO se firma. El aviso de SmartScreen
# que verá el usuario está documentado en español en
# LEEME-Primeros-pasos.txt.
# No se finge que el .exe esté firmado.
set -euo pipefail

PLATAFORMA="${1:-linux}"
SALIDA=""

while [ $# -gt 0 ]; do
	case "$1" in
		linux|windows) PLATAFORMA="$1"; shift ;;
		--salida) SALIDA="$2"; shift 2 ;;
		--salida=*) SALIDA="${1#--salida=}"; shift ;;
		*) echo "Uso: $0 [linux|windows] [--salida DIR]" >&2; exit 2 ;;
	esac
done

SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="$SOURCE_DIR/dist"

if [ "$PLATAFORMA" = windows ]; then
	PKG="untrunc-mejorado-windows-x64"
	EXE_CLI="untrunc.exe"
	EXE_GUI="untrunc-gui.exe"
else
	PKG="untrunc-mejorado-linux-x64"
	EXE_CLI="untrunc"
	EXE_GUI="untrunc-gui"
fi

STAGE="$DIST_DIR/$PKG"

# Salida del ZIP: si existe ../phase3 (el informe de la Fase 3),
# el ZIP va allí; si no, queda en dist/ junto al staging.
if [ -z "$SALIDA" ]; then
	if [ -d "$SOURCE_DIR/../phase3" ]; then
		SALIDA="$SOURCE_DIR/../phase3"
	else
		SALIDA="$DIST_DIR"
	fi
fi
mkdir -p "$SALIDA"
ZIP_OUT="$SALIDA/$PKG.zip"

echo "== Untrunc-mejorado: empaquetado ($PLATAFORMA) =="
echo "Código:      $SOURCE_DIR"
echo "Staging:     $STAGE"
echo "ZIP:         $ZIP_OUT"

# --- 1. Requisitos previos -----------------------------------------------
if [ "$PLATAFORMA" = windows ]; then
	[ -f "$SOURCE_DIR/$EXE_CLI" ] || { echo "ERROR: falta $EXE_CLI. Compila antes: make IS_RELEASE=1" >&2; exit 1; }
	[ -f "$SOURCE_DIR/$EXE_GUI" ] || { echo "ERROR: falta $EXE_GUI. Compila antes: make untrunc-gui LIBUI_STATIC=1 IS_RELEASE=1 (necesita libui)" >&2; exit 1; }
	command -v ntldd >/dev/null 2>&1 || { echo "ERROR: falta ntldd (en MSYS2: pacboy ntldd:p). Es quien lista las DLL que necesita el .exe." >&2; exit 1; }
	: "${MINGW_PREFIX:=/mingw64}"
	[ -d "$MINGW_PREFIX/bin" ] || { echo "ERROR: no existe \$MINGW_PREFIX/bin ($MINGW_PREFIX). Ejecuta el script en la terminal MINGW64 de MSYS2." >&2; exit 1; }
else
	[ -f "$SOURCE_DIR/$EXE_CLI" ] || { echo "ERROR: falta $EXE_CLI. Compila antes: make -j4" >&2; exit 1; }
fi

for f in COPYING README.md README-ESPANOL.md LEEME-Primeros-pasos.txt \
	profiles/profiles.json \
	src/ai/onnx/modelo_base/modelo.onnx \
	src/ai/onnx/modelo_base/pesos.json \
	src/ai/onnx/modelo_base/model_info.json \
	src/ai/onnx/train_onnx.py; do
	[ -f "$SOURCE_DIR/$f" ] || { echo "ERROR: falta $f en el código." >&2; exit 1; }
done

# --- 2. Staging: carpeta dist/<paquete>/ ----------------------------------
rm -rf "$STAGE"
mkdir -p "$STAGE/profiles" "$STAGE/models/onnx_base" "$STAGE/tools"

# Ejecutables (ya compilados; NO se recompila nada aquí).
cp -v "$SOURCE_DIR/$EXE_CLI" "$STAGE/$EXE_CLI"
if [ "$PLATAFORMA" = windows ]; then
	cp -v "$SOURCE_DIR/$EXE_GUI" "$STAGE/$EXE_GUI"
else
	# En Linux la GUI no se compila en este entorno (falta libui):
	# el ZIP de prueba lleva solo el CLI. Si algún día hay un
	# untrunc-gui compilado en Linux, se incluye automáticamente.
	if [ -f "$SOURCE_DIR/$EXE_GUI" ]; then
		cp -v "$SOURCE_DIR/$EXE_GUI" "$STAGE/$EXE_GUI"
	else
		echo "AVISO: no hay untrunc-gui en Linux (falta libui); el ZIP de prueba lleva solo el CLI."
		echo "       La GUI es Windows: se incluye solo en el paquete windows."
	fi
fi

# DLLs de FFmpeg (solo Windows): las que necesita untrunc-gui.exe según
# ntldd, copiadas desde $MINGW_PREFIX/bin con el nombre en mayúsculas,
# exactamente como hace .github/workflows/release.yml. En Linux no se
# empaquetan .so del sistema: el binario usa el FFmpeg del sistema
# (libavformat/libavcodec/libavutil instalados con apt).
if [ "$PLATAFORMA" = windows ]; then
	echo "== DLLs (ntldd -R $EXE_GUI, desde $MINGW_PREFIX/bin) =="
	ntldd -R "$SOURCE_DIR/$EXE_GUI" | awk '{print $1}' | sort -u | while read -r name; do
		src="$MINGW_PREFIX/bin/$name"
		[ -f "$src" ] || { echo "  (sin copiar, no está en \$MINGW_PREFIX/bin: $name)"; continue; }
		cp -v "$src" "$STAGE/$(basename "$name" | tr '[:lower:]' '[:upper:]')"
	done
fi

# ffmpeg.exe/ffprobe.exe y Python 3 + numpy incluidos (solo Windows),
# para que el usuario no tenga que instalar nada: el modo sin
# referencia, la conversión de formatos y la miniatura usan ffmpeg;
# la pestaña Entrenar y el aprendizaje automático usan Python + numpy.
# El programa los antepone a su PATH al arrancar
# (upath::usarHerramientasIncluidas). Las DLL se buscan con objdump,
# de forma recursiva, entre las de $MINGW_PREFIX/bin.
if [ "$PLATAFORMA" = windows ]; then
	declare -A DLL_VISTA
	# copiar_dlls <binario> <destino> [mayus]: copia las DLL de
	# $MINGW_PREFIX/bin que necesita el binario (y las de esas DLL).
	copiar_dlls() {
		local bin="$1" dest="$2" mayus="${3:-}" d low nombre
		for d in $(objdump -p "$bin" 2>/dev/null | awk '/DLL Name:/{print $3}'); do
			low=${d,,}
			[ -n "${DLL_VISTA[$dest|$low]:-}" ] && continue
			DLL_VISTA[$dest|$low]=1
			[ -f "$MINGW_PREFIX/bin/$d" ] || continue
			nombre="$d"
			[ -n "$mayus" ] && nombre=$(echo "$d" | tr '[:lower:]' '[:upper:]')
			[ -f "$dest/$nombre" ] || cp "$MINGW_PREFIX/bin/$d" "$dest/$nombre"
			copiar_dlls "$MINGW_PREFIX/bin/$d" "$dest" "$mayus"
		done
	}

	echo "== ffmpeg.exe y ffprobe.exe (desde $MINGW_PREFIX/bin) =="
	for e in ffmpeg.exe ffprobe.exe; do
		[ -f "$MINGW_PREFIX/bin/$e" ] || { echo "ERROR: falta $MINGW_PREFIX/bin/$e (pacman -S mingw-w64-x86_64-ffmpeg)." >&2; exit 1; }
		cp -v "$MINGW_PREFIX/bin/$e" "$STAGE/$e"
		copiar_dlls "$MINGW_PREFIX/bin/$e" "$STAGE" mayus
	done

	echo "== Python 3 + numpy (desde $MINGW_PREFIX) =="
	PY_BIN="$MINGW_PREFIX/bin/python.exe"
	[ -f "$PY_BIN" ] || { echo "ERROR: falta $PY_BIN (pacman -S mingw-w64-x86_64-python mingw-w64-x86_64-python-numpy)." >&2; exit 1; }
	PYV=$("$PY_BIN" -c 'import sys;print(f"{sys.version_info[0]}.{sys.version_info[1]}")')
	PY_LIB="$MINGW_PREFIX/lib/python$PYV"
	[ -d "$PY_LIB/site-packages/numpy" ] || { echo "ERROR: falta numpy (pacman -S mingw-w64-x86_64-python-numpy)." >&2; exit 1; }
	PY_DEST="$STAGE/python"
	mkdir -p "$PY_DEST/bin" "$PY_DEST/lib/python$PYV/site-packages"
	cp "$PY_BIN" "$PY_DEST/bin/python.exe"
	copiar_dlls "$PY_BIN" "$PY_DEST/bin"
	# Biblioteca estándar sin pruebas, IDLE, tkinter ni cachés.
	tar -C "$PY_LIB" -cf - \
		--exclude='./site-packages' --exclude='./test' --exclude='./idlelib' \
		--exclude='./tkinter' --exclude='./turtledemo' --exclude='./ensurepip' \
		--exclude='./lib2to3' --exclude='./config-*' \
		--exclude='__pycache__' --exclude='*/tests' \
		--exclude='_tkinter*.pyd' --exclude='_curses*.pyd' . \
		| tar -C "$PY_DEST/lib/python$PYV" -xf -
	tar -C "$PY_LIB/site-packages" -cf - --exclude='__pycache__' --exclude='*/tests' --exclude='*.a' --exclude='*/include' numpy \
		| tar -C "$PY_DEST/lib/python$PYV/site-packages" -xf -
	while read -r p; do copiar_dlls "$p" "$PY_DEST/bin"; done \
		< <(find "$PY_DEST/lib/python$PYV" -name '*.pyd')
	echo "Python $PYV incluido: $(du -sh "$PY_DEST" | cut -f1)"
fi

# Datos: perfiles de cámaras, modelo base ONNX y script de entrenamiento.
# Rutas DENTRO del ZIP (las que busca el código, ver onnx_provider.cpp
# findTrainScript/findBaseModelDir y profile_db.cpp loadProfiles):
#   profiles/profiles.json, models/onnx_base/, tools/train_onnx.py
cp -v "$SOURCE_DIR/profiles/profiles.json" "$STAGE/profiles/profiles.json"
mkdir -p "$STAGE/profiles/plantillas"
cp -v "$SOURCE_DIR/profiles/plantillas/README.md" "$STAGE/profiles/plantillas/README.md"
cp -v "$SOURCE_DIR/src/ai/onnx/modelo_base/modelo.onnx" \
      "$SOURCE_DIR/src/ai/onnx/modelo_base/pesos.json" \
      "$SOURCE_DIR/src/ai/onnx/modelo_base/model_info.json" \
      "$STAGE/models/onnx_base/"
cp -v "$SOURCE_DIR/src/ai/onnx/train_onnx.py" "$STAGE/tools/train_onnx.py"

# Documentación y licencia.
cp -v "$SOURCE_DIR/README.md" "$SOURCE_DIR/README-ESPANOL.md" \
      "$SOURCE_DIR/LEEME-Primeros-pasos.txt" "$STAGE/"
cp -v "$SOURCE_DIR/COPYING" "$STAGE/COPYING"

# Licencias de todo lo que el ZIP trae de terceros (FFmpeg, libui, y los
# paquetes de MSYS2: Python, numpy, OpenBLAS, runtime de GCC...). Repartir
# esos binarios obliga a incluir sus licencias y decir dónde está su código.
if [ "$PLATAFORMA" = windows ]; then
	echo "== Licencias de terceros =="
	LIC="$STAGE/LICENCIAS"
	mkdir -p "$LIC/ffmpeg" "$LIC/msys2"
	# FFmpeg es la compilación de Zeranoe (no es de MSYS2): su LICENSE.txt y
	# su README.txt (configuración y bibliotecas incluidas) se guardan al
	# instalarla en $MINGW_PREFIX/share/licenses/ffmpeg-zeranoe/.
	FF_LIC="$MINGW_PREFIX/share/licenses/ffmpeg-zeranoe"
	for f in LICENSE.txt README.txt; do
		[ -f "$FF_LIC/$f" ] || { echo "ERROR: falta $FF_LIC/$f (cópialo del ZIP de FFmpeg de Zeranoe)." >&2; exit 1; }
		cp "$FF_LIC/$f" "$LIC/ffmpeg/$f"
	done
	cp "$SOURCE_DIR/docs/licencias/libui-LICENSE.txt" "$LIC/libui-LICENSE.txt"

	# Paquete de MSYS2 dueño de cada binario incluido (FFmpeg se salta:
	# pacman podría atribuirlo a otro ffmpeg instalado antes).
	declare -A PAQ_MSYS
	BIN_LISTA=$( { find "$STAGE" -maxdepth 1 -iname '*.dll'; find "$STAGE/python/bin" -iname '*.dll' -o -iname '*.exe'; } )
	while read -r f; do
		[ -n "$f" ] || continue
		b=$(basename "$f"); low=${b,,}
		case "$low" in ffmpeg.exe|ffprobe.exe|av*.dll|sw*.dll|postproc*.dll) continue;; esac
		[ -f "$MINGW_PREFIX/bin/$b" ] || continue
		p=$(pacman -Qqo "$MINGW_PREFIX/bin/$b" 2>/dev/null) || continue
		PAQ_MSYS[$p]=1
	done <<< "$BIN_LISTA"
	# Python (biblioteca estándar y .pyd) y numpy.
	for f in "$MINGW_PREFIX/bin/python.exe" "$PY_LIB/site-packages/numpy/__init__.py"; do
		p=$(pacman -Qqo "$f" 2>/dev/null) && PAQ_MSYS[$p]=1
	done

	{
		echo "LICENCIAS DE LOS COMPONENTES INCLUIDOS EN ESTE ZIP"
		echo "=================================================="
		echo
		echo "untrunc-mejorado se distribuye bajo la GNU GPL versión 2 (archivo COPYING)."
		echo "Su código fuente: https://github.com/agarciaaa/untrunc-mejorado"
		echo
		echo "Para que el programa funcione sin instalar nada, el ZIP trae además"
		echo "programas de terceros, cada uno con su propia licencia (en esta carpeta):"
		echo
		echo "1) FFmpeg 3.3.4 — ffmpeg.exe, ffprobe.exe y las DLL AV*, SW*, POSTPROC*."
		echo "   Compilación de Zeranoe (ffmpeg.zeranoe.com, ya cerrado), con --enable-gpl"
		echo "   y --enable-version3: licencia GNU GPL versión 3 (ffmpeg/LICENSE.txt)."
		echo "   Configuración exacta y bibliotecas incluidas (x264, x265, ...) con sus"
		echo "   versiones y webs: ffmpeg/README.txt."
		echo "   Código fuente de FFmpeg 3.3.4: https://ffmpeg.org/releases/ffmpeg-3.3.4.tar.xz"
		echo "   Copia del paquete original: https://web.archive.org/https://ffmpeg.zeranoe.com/builds/win64/shared/ffmpeg-3.3.4-win64-shared.zip"
		echo
		echo "2) libui alpha4.1 — interfaz gráfica, enlazada dentro de untrunc-gui.exe."
		echo "   Licencia MIT (libui-LICENSE.txt). Código: https://github.com/andlabs/libui/tree/alpha4.1"
		echo
		echo "3) Paquetes de MSYS2 (Python, numpy, OpenBLAS, runtime de GCC y otras DLL)."
		echo "   Licencia de cada uno en msys2/<paquete>/. Código fuente y receta de"
		echo "   compilación: https://packages.msys2.org/package/<paquete> y"
		echo "   https://github.com/msys2/MINGW-packages ; archivos de código fuente:"
		echo "   https://repo.msys2.org/mingw/sources/"
		echo
		printf '   %-40s %-22s %s\n' "PAQUETE" "VERSIÓN" "LICENCIA"
		for p in $(printf '%s\n' "${!PAQ_MSYS[@]}" | sort); do
			ver=$(pacman -Q "$p" | awk '{print $2}')
			lic=$(LC_ALL=C pacman -Qi "$p" | sed -n 's/^Licenses *: *//p')
			printf '   %-40s %-22s %s\n' "$p" "$ver" "$lic"
			corto=${p#mingw-w64-x86_64-}
			while read -r lf; do
				[ -f "$lf" ] || continue
				mkdir -p "$LIC/msys2/$corto"
				cp "$lf" "$LIC/msys2/$corto/"
			done < <(pacman -Qlq "$p" | grep '/share/licenses/' | grep -v '/$' || true)
		done
		echo
		echo "Si algún enlace deja de funcionar, abre una incidencia en el repositorio"
		echo "y se indicará otra forma de conseguir el código fuente."
		echo
		echo "English: this ZIP bundles FFmpeg 3.3.4 (Zeranoe build, GPLv3), libui (MIT)"
		echo "and MSYS2 packages (Python, numpy and their DLLs). Their licenses are in this"
		echo "folder; source code locations are listed above."
	} > "$LIC/LEEME-LICENCIAS.txt"
	echo "Licencias: $(ls "$LIC/msys2" | wc -l) paquetes de MSYS2 + FFmpeg + libui"
fi

# --- 3. Verificación del paquete (falla si algo no cuadra) ----------------
echo "== Verificación del paquete =="

# 3a. Archivos obligatorios presentes.
if [ "$PLATAFORMA" = windows ]; then
	for f in "$EXE_CLI" "$EXE_GUI" COPYING README.md README-ESPANOL.md \
		LEEME-Primeros-pasos.txt profiles/profiles.json \
		models/onnx_base/modelo.onnx models/onnx_base/pesos.json \
		models/onnx_base/model_info.json tools/train_onnx.py \
		ffmpeg.exe ffprobe.exe python/bin/python.exe \
		LICENCIAS/LEEME-LICENCIAS.txt LICENCIAS/ffmpeg/LICENSE.txt \
		LICENCIAS/libui-LICENSE.txt; do
		[ -f "$STAGE/$f" ] || { echo "ERROR: falta en el paquete: $f" >&2; exit 1; }
	done
	# ffmpeg y Python incluidos deben arrancar (-B: sin escribir .pyc).
	"$STAGE/ffmpeg.exe" -hide_banner -version >/dev/null 2>&1 || { echo "ERROR: el ffmpeg.exe del paquete no arranca." >&2; exit 1; }
	(cd / && PYTHONHOME= PYTHONPATH= "$STAGE/python/bin/python.exe" -B -c 'import numpy, json, hashlib, argparse') \
		|| { echo "ERROR: el Python del paquete no arranca o no encuentra numpy." >&2; exit 1; }
	# Al menos una DLL de FFmpeg (AVCODEC/AVFORMAT/AVUTIL) debe estar.
	ls "$STAGE"/AV*.DLL >/dev/null 2>&1 || { echo "ERROR: no se copió ninguna DLL de FFmpeg (AV*.DLL)." >&2; exit 1; }
else
	for f in "$EXE_CLI" COPYING README.md README-ESPANOL.md \
		LEEME-Primeros-pasos.txt profiles/profiles.json \
		models/onnx_base/modelo.onnx models/onnx_base/pesos.json \
		models/onnx_base/model_info.json tools/train_onnx.py; do
		[ -f "$STAGE/$f" ] || { echo "ERROR: falta en el paquete: $f" >&2; exit 1; }
	done
fi

# 3b. PROHIBIDO: ningún vídeo, ningún dataset de entrenamiento, ningún
#     resto de compilación, ningún archivo con datos de Angel.
#     (El dataset con ventanas de entrenamiento vive en
#     ~/.config/untrunc/entrenamiento/ del usuario, nunca en el ZIP;
#     aquí no hay ninguno, y si lo hubiera el empaquetado debe fallar.)
PROHIBIDOS=$(find "$STAGE" -type f \( \
	-iname '*.mp4' -o -iname '*.mov' -o -iname '*.m4v' -o -iname '*.3gp' \
	-o -iname '*.mkv' -o -iname '*.avi' -o -iname '*.webm' -o -iname '*.wmv' \
	-o -iname '*.flv' -o -iname '*.mpg' -o -iname '*.mts' -o -iname '*.m2ts' \
	-o -iname '*.mp3' -o -iname '*.wav' -o -iname '*.npz' \
	-o -iname '*.pyc' -o -iname 'dataset*' -o -iname '*entrenamiento*' \
	-o -iname '*.o' -o -iname '*.a' \) -print)
if [ -n "$PROHIBIDOS" ]; then
	echo "ERROR: archivos prohibidos en el paquete (vídeo/dataset/restos):" >&2
	echo "$PROHIBIDOS" >&2
	exit 1
fi

# 3c. COPYING intacto: el del paquete debe ser byte a byte el del repo.
if ! cmp -s "$SOURCE_DIR/COPYING" "$STAGE/COPYING"; then
	echo "ERROR: COPYING del paquete difiere del COPYING del código. No se modifica la licencia." >&2
	exit 1
fi

# 3d. Sin rutas personales del equipo donde se armó el paquete
#     (ni en textos ni en el JSON del modelo).
if [ -n "${HOME:-}" ] && grep -rF -- "$HOME" "$STAGE" --include='*.txt' --include='*.md' --include='*.json' --include='*.py' -q 2>/dev/null; then
	echo "ERROR: hay rutas personales ($HOME) en los archivos del paquete." >&2
	exit 1
fi

# 3e. Sin claves ni secretos (por si acaso: el paquete no debe llevar
#     ninguna API key ni token).
if grep -riE -- 'api[_-]?key\s*=\s*["'"'"'][^"'"'"']{8,}|bearer [A-Za-z0-9._-]{16,}|UNTRUNC_AI_API_KEY\s*=\s*\S+' \
	"$STAGE" --include='*.txt' --include='*.md' --include='*.json' --include='*.py' -q 2>/dev/null; then
	echo "ERROR: posible clave/API key en los archivos del paquete." >&2
	exit 1
fi

# --- 4. Contenido y ZIP -----------------------------------------------------
echo "== Contenido del paquete =="
find "$STAGE" -type f -printf '%12s  %p\n' | sort -k2
echo "Total staging: $(du -sh "$STAGE" | cut -f1)"

rm -f "$ZIP_OUT"
if command -v zip >/dev/null 2>&1; then
	(cd "$DIST_DIR" && zip -qr "$ZIP_OUT" "$PKG")
else
	python3 -m zipfile -c "$ZIP_OUT" "$STAGE"
fi

echo "== ZIP creado =="
ls -lh "$ZIP_OUT"
sha256sum "$ZIP_OUT"
echo "Contenido del ZIP:"
unzip -l "$ZIP_OUT" 2>/dev/null || python3 -m zipfile -l "$ZIP_OUT"
echo "OK: paquete $PKG listo."
