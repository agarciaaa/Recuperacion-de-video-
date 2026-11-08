// multi_open.h — Punto 10 (2026-11-07): selección múltiple de archivos.
//
// libui no tiene diálogo de selección múltiple: `uiOpenFile` solo
// devuelve UN archivo. En Windows se usa el diálogo nativo
// (`GetOpenFileNameW` con `OFN_ALLOWMULTISELECT`, ver
// `Gui::openVideoFilesMulti()` en gui.cpp); en Linux/macOS se sigue
// usando `uiOpenFile` (un archivo).
//
// Este archivo solo tiene lógica PURA, sin API de Windows ni de libui,
// para poder probarla en Linux con el código real:
//  - `parseMultiSelectBuffer`: el búfer que devuelve GetOpenFileName
//    con OFN_ALLOWMULTISELECT. Con un archivo es la ruta completa;
//    con varios, el primer string es la carpeta y los siguientes son
//    los nombres (sin ruta). Devuelve rutas completas.
//  - `agregarSinDuplicados`: añade los nuevos a la lista sin repetir
//    los que ya están ni los duplicados dentro del mismo lote.
// Se usa `wchar_t` porque el diálogo nativo es la versión W
// (UTF-16); la conversión a UTF-8 se hace en gui.cpp, para que las
// rutas con acentos lleguen bien al resto del programa (que trabaja
// en UTF-8, como libui). En Linux `wchar_t` es de 4 bytes y en
// Windows de 2, pero la lógica solo compara con L'\0' y L'\\', así
// que es la misma en los dos.
#pragma once

#include <algorithm>
#include <string>
#include <vector>

namespace MultiOpen {

// Parsea el búfer de GetOpenFileName con OFN_ALLOWMULTISELECT.
// `buf_len` es el tamaño del búfer en wchar_t (incluye el NUL final).
// Si el búfer está vacío o solo tiene NULs, devuelve una lista vacía.
inline std::vector<std::wstring> parseMultiSelectBuffer(const wchar_t* buf,
                                                        size_t buf_len) {
	std::vector<std::wstring> out;
	if (!buf || buf_len == 0 || buf[0] == L'\0') return out;
	std::vector<std::wstring> partes;
	size_t i = 0;
	while (i < buf_len && buf[i] != L'\0') {
		size_t inicio = i;
		while (i < buf_len && buf[i] != L'\0') i++;
		partes.push_back(std::wstring(buf + inicio, i - inicio));
		i++;  // saltar el NUL que separa los strings
	}
	if (partes.empty()) return out;
	if (partes.size() == 1) {
		// Un solo archivo: el búfer ya es la ruta completa.
		out.push_back(partes[0]);
		return out;
	}
	// Varios archivos: la primera parte es la carpeta, el resto son
	// nombres de archivo sin ruta.
	std::wstring dir = partes[0];
	if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/')
		dir += L'\\';
	for (size_t k = 1; k < partes.size(); k++)
		out.push_back(dir + partes[k]);
	return out;
}

// Añade `nuevos` a `lista` sin duplicar: se salta lo que ya está en
// la lista y lo que se repite dentro del mismo lote.
inline void agregarSinDuplicados(std::vector<std::string>& lista,
                                 const std::vector<std::string>& nuevos) {
	for (const auto& p : nuevos) {
		if (std::find(lista.begin(), lista.end(), p) == lista.end())
			lista.push_back(p);
	}
}

}  // namespace MultiOpen
