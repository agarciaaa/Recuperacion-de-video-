/*
	Untrunc-mejorado - shell_util.h

	Utilidades comunes para lanzar programas externos (ffmpeg,
	ffprobe, curl...) con std::system() de forma portable.

	En Windows, std::system() usa cmd.exe, que:
	  - NO entiende las comillas simples POSIX ('ruta'): las pasa
	    tal cual como parte del argumento, y el programa no
	    encuentra el archivo;
	  - NO tiene /dev/null: "> /dev/null" falla con "El sistema no
	    puede encontrar la ruta especificada" y el comando entero
	    devuelve error (aunque ffmpeg este instalado);
	  - NO tiene "command -v" (se usa "where").

	Antes cada archivo tenia su propia copia de shellQuote() y
	escribia "/dev/null" a mano; los de formatos.cpp y
	moov_synthesizer.cpp eran solo POSIX, por lo que convertir a
	MP4 y el modo sin referencia fallaban siempre en Windows.

	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#ifndef UNTR_SHELL_UTIL_H
#define UNTR_SHELL_UTIL_H

#include <cstdlib>
#include <string>

namespace shell {

// Dispositivo nulo de la shell que usa std::system().
#ifdef _WIN32
inline const char* const kNullDevice = "NUL";
#else
inline const char* const kNullDevice = "/dev/null";
#endif

// Entrecomilla un argumento para la shell de std::system().
// Windows (cmd.exe): comillas dobles. Un nombre de archivo de
// Windows no puede contener comillas dobles, asi que basta con
// escaparlas por si acaso.
// POSIX (sh): comillas simples; una comilla simple dentro se
// escribe como '\'' (cierra, comilla escapada, reabre).
inline std::string quote(const std::string& s) {
#ifdef _WIN32
	std::string out = "\"";
	for (char c : s) {
		if (c == '"') out += "\\\"";
		else out += c;
	}
	out += "\"";
	return out;
#else
	std::string out = "'";
	for (char c : s) {
		if (c == '\'') out += "'\\''";
		else out += c;
	}
	out += "'";
	return out;
#endif
}

// " 2>NUL" / " 2>/dev/null": descarta stderr.
inline std::string discardStderr() {
	return std::string(" 2>") + kNullDevice;
}

// " >NUL 2>&1" / " >/dev/null 2>&1": descarta stdout y stderr.
inline std::string discardAll() {
	return std::string(" >") + kNullDevice + " 2>&1";
}

// true si el programa esta en el PATH. `name` debe ser un nombre
// simple y fijo del codigo (p. ej. "ffmpeg"), nunca texto del usuario.
inline bool commandAvailable(const std::string& name) {
#ifdef _WIN32
	std::string cmd = "where " + name + discardAll();
#else
	std::string cmd = "command -v " + name + discardAll();
#endif
	return std::system(cmd.c_str()) == 0;
}

} // namespace shell

#endif // UNTR_SHELL_UTIL_H
