/*
	Untrunc-mejorado - upath.h

	Rutas y comandos con acentos, ñ y cualquier caracter Unicode.

	Dentro del programa TODAS las rutas son std::string en UTF-8:
	en Windows, common.cpp convierte argv con CommandLineToArgvW y
	la GUI (libui) devuelve UTF-8. Pero en Windows la biblioteca
	estandar interpreta un std::string como texto en la pagina de
	codigos ANSI, no como UTF-8, en:
	  - fs::path(std::string), fs::exists(std::string), ...
	  - std::ifstream / std::ofstream con std::string
	  - fs::path::string()
	  - std::system() y popen()
	Resultado: "C:\Users\José\Vídeos\a.mp4" no se encontraba
	("formato desconocido", "no existe"...), y las rutas temporales
	de un usuario con tilde en el nombre tampoco funcionaban.

	Reglas para el codigo nuevo:
	  - std::string -> fs::path:  upath::toPath(s)   (nunca fs::path(s))
	  - fs::path -> std::string:  upath::fromPath(p) (nunca p.string())
	  - std::ifstream/ofstream:   pasarles un fs::path de toPath()
	  - comandos externos:        upath::system / upath::openPipe
	En Linux/macOS todo esto es equivalente a lo estandar.

	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#ifndef UNTR_UPATH_H
#define UNTR_UPATH_H

#include <cstdio>
#include <filesystem>
#include <string>

namespace upath {

// UTF-8 -> ruta del sistema.
std::filesystem::path toPath(const std::string& utf8);

// Ruta del sistema -> UTF-8.
std::string fromPath(const std::filesystem::path& p);

// std::system() con el comando en UTF-8 (en Windows: _wsystem).
int system(const std::string& utf8_cmd);

// popen()/pclose() con el comando en UTF-8 (en Windows: _wpopen).
// No se llaman popen/pclose porque MinGW los define como macros.
FILE* openPipe(const std::string& utf8_cmd, const char* mode);
int closePipe(FILE* f);

// Variable de entorno en UTF-8 ("" si no existe). En Windows
// std::getenv devuelve texto ANSI: USERPROFILE de "José" no
// serviria como ruta UTF-8.
std::string getenv(const char* name);

// Ruta completa del ejecutable en UTF-8 ("" si no se sabe).
std::string executablePath();

// Windows: antepone al PATH de este proceso la carpeta del programa
// (si trae ffmpeg.exe) y su Python incluido (python\bin), para que
// ffmpeg, ffprobe y python del paquete portable se usen sin instalar
// nada. Si existen esas carpetas, ganan a cualquier otra copia del
// sistema. Sin ellas (desarrollo) o fuera de Windows no hace nada.
void usarHerramientasIncluidas();

} // namespace upath

#endif // UNTR_UPATH_H
