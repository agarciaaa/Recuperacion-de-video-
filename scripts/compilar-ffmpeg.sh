#!/bin/bash
# Compila FFmpeg para el ZIP de Windows en MSYS2 MINGW64 y lo instala en
# $MINGW_PREFIX (DLL, ffmpeg.exe, ffprobe.exe, cabeceras y bibliotecas de
# importación). Lo usa la CI (.github/workflows/release.yml); en local
# sirve igual.
#
# Licencia: --enable-gpl SIN --enable-version3, con x264 y x265 (las dos
# GPL-2.0-or-later). El resultado es GPL-2.0-or-later, compatible con la
# GPL-2.0 de untrunc. No añadir bibliotecas que exijan --enable-version3
# (opencore-amr, vo-amrwbenc, mbedtls, gmp, ...) ni --enable-nonfree.
#
# Uso: scripts/compilar-ffmpeg.sh [directorio-de-trabajo]
# Necesita: pacman -S make diffutils mingw-w64-x86_64-{gcc,nasm,pkgconf,x264,x265,zlib}
#
# La compilación se instala primero en <trabajo>/destino (que la CI guarda
# en caché) y de ahí se copia a $MINGW_PREFIX. Si <trabajo>/destino ya
# existe, no se recompila.
set -euo pipefail

FF_VER="7.1.5"
FF_SHA256="de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f"
: "${MINGW_PREFIX:?Ejecuta este script en la terminal MINGW64 de MSYS2.}"
TRABAJO="$(mkdir -p "${1:-$PWD/.ffmpeg-build}" && cd "${1:-$PWD/.ffmpeg-build}" && pwd)"
DESTINO="$TRABAJO/destino"
LIC_REL="share/licenses/ffmpeg-gpl2"

# Configuración exacta: se guarda junto a la licencia para LICENCIAS/ del ZIP.
CONFIG=(
	--prefix="$MINGW_PREFIX"
	--enable-shared --disable-static
	--enable-gpl --enable-libx264 --enable-libx265
	--disable-autodetect --enable-zlib
	--disable-network --disable-doc --disable-debug
	--disable-ffplay --disable-postproc
)

if [ ! -x "$DESTINO$MINGW_PREFIX/bin/ffmpeg.exe" ]; then
	cd "$TRABAJO"
	tarball="ffmpeg-$FF_VER.tar.xz"
	if [ ! -f "$tarball" ]; then
		curl -fsSL --retry 5 --retry-delay 10 -o "$tarball.tmp" "https://ffmpeg.org/releases/$tarball"
		mv "$tarball.tmp" "$tarball"
	fi
	echo "$FF_SHA256  $tarball" | sha256sum -c -
	rm -rf "ffmpeg-$FF_VER" "$DESTINO"
	tar -xf "$tarball"
	cd "ffmpeg-$FF_VER"
	./configure "${CONFIG[@]}"
	make -j"$(nproc)"
	make install DESTDIR="$DESTINO"

	# Licencia, configuración y código fuente para LICENCIAS/ffmpeg/ del ZIP
	# y para adjuntar el código a cada versión publicada.
	LIC="$DESTINO$MINGW_PREFIX/$LIC_REL"
	mkdir -p "$LIC"
	cp COPYING.GPLv2 LICENSE.md "$LIC/"
	{
		echo "FFmpeg $FF_VER compilado para untrunc-mejorado: licencia GPL-2.0-or-later"
		echo "(--enable-gpl sin --enable-version3; ver COPYING.GPLv2 y LICENSE.md)."
		echo
		echo "Código fuente exacto: https://ffmpeg.org/releases/$tarball"
		echo "SHA256: $FF_SHA256"
		echo "(también se adjunta a cada versión publicada en GitHub)."
		echo
		echo "Configuración:"
		printf '  %s\n' "${CONFIG[@]}"
		echo
		echo "Bibliotecas externas: x264 y x265 (paquetes de MSYS2, ver ../msys2/)."
		echo "Compilado con el guion scripts/compilar-ffmpeg.sh del repositorio."
	} > "$LIC/CONFIGURACION.txt"
	cp "../$tarball" "$LIC/"
fi

# Copia a $MINGW_PREFIX (cp -r sobre lo que haya).
cp -r "$DESTINO$MINGW_PREFIX/." "$MINGW_PREFIX/"
"$MINGW_PREFIX/bin/ffmpeg.exe" -hide_banner -version | head -1
