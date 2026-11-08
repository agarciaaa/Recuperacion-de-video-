/*
	Untrunc-mejorado - memoria.h

	Memoria de reparaciones: recuerda, en la PC del usuario, que
	estrategia (normal, -s, -dyn, -s -dyn) recupero mas con cada tipo
	de camara, para probarla PRIMERO la proxima vez.

	La "firma" de una camara sale de la cabecera de la referencia (o
	plantilla): codec de video, resolucion, marca del ftyp y codecs de
	todas las pistas. No se guarda ninguna ruta, nombre de archivo ni
	dato del video: solo la firma y contadores.

	Archivo: <HOME o USERPROFILE>/.config/untrunc/memoria_reparaciones.tsv
	(una linea por firma y estrategia: firma<TAB>s<TAB>dyn<TAB>victorias<TAB>fecha).

	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#ifndef UNTRUNC_MEMORIA_H
#define UNTRUNC_MEMORIA_H

#include <string>

namespace memoria {

// Firma de camara de un MP4 sano o plantilla ("" si no se puede leer).
std::string firmaReferencia(const std::string& ref_path);

struct Recuerdo {
	bool hay = false;
	bool s = false;       // -s
	bool dyn = false;     // -dyn
	int victorias = 0;    // veces que esta estrategia fue la mejor
};

// La estrategia que mas veces gano con esta firma.
Recuerdo consultar(const std::string& firma);

// Apunta que (s, dyn) fue la mejor estrategia para esta firma.
// Devuelve false si no se pudo guardar (no es grave).
bool anotar(const std::string& firma, bool s, bool dyn);

std::string rutaMemoria();

// Solo para pruebas: usar otro archivo ("" = el normal).
void usarArchivo(const std::string& ruta);

} // namespace memoria

#endif
