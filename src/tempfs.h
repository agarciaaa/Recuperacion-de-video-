/*
	Untrunc-mejorado - tempfs.h

	Carpetas temporales con nombre UNICO por operacion.

	Antes cada modulo usaba una carpeta fija en %TEMP% / /tmp
	("untrunc-ai-ext", "untrunc-ai-lmstudio", "untrunc-ai-ref",
	"untrunc-codecs.txt"...). Dos operaciones a la vez (dos ventanas,
	o la GUI convirtiendo mientras repara) se pisaban los archivos,
	y en Linux otro usuario podia crear antes esa carpeta fija
	(con permisos suyos o un enlace simbolico) en /tmp.

	createUniqueDir() crea una carpeta NUEVA con un nombre aleatorio:
	fs::create_directory es atomico y devuelve false si ya existia,
	asi que nunca se reutiliza una carpeta ajena. En POSIX se deja
	con permisos 0700 (solo el usuario). En Windows %TEMP% ya es
	privado de cada usuario.

	ScopedDir la borra con todo su contenido al salir del ambito,
	tambien en los return anticipados (antes, si fallaba la escritura
	de un archivo, el curlrc con la API key podia quedarse en disco).

	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#ifndef UNTR_TEMPFS_H
#define UNTR_TEMPFS_H

#include <filesystem>
#include <string>

namespace tempfs {

// Crea <temp>/untrunc-<prefijo>-<aleatorio>/ y devuelve su ruta.
// Si no se puede, devuelve una ruta vacia y rellena ec.
std::filesystem::path createUniqueDir(const std::string& prefix, std::error_code& ec);

class ScopedDir {
public:
	explicit ScopedDir(const std::string& prefix);
	~ScopedDir();
	ScopedDir(const ScopedDir&) = delete;
	ScopedDir& operator=(const ScopedDir&) = delete;

	bool ok() const { return !path_.empty(); }
	const std::filesystem::path& path() const { return path_; }
	std::filesystem::path operator/(const std::string& name) const { return path_ / name; }

	// Deja de ser responsable de borrarla (la borrara otro).
	std::filesystem::path release();

private:
	std::filesystem::path path_;
};

// Borra una carpeta temporal y su contenido (sin lanzar).
void removeDir(const std::filesystem::path& dir);

} // namespace tempfs

#endif // UNTR_TEMPFS_H
