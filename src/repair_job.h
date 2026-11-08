/*
	Untrunc-mejorado (Fase 1) - repair_job.h

	Logica compartida de reparacion para el CLI (main.cpp) y la GUI
	(gui.cpp): pre-vuelo, reintento automatico con -s/-dyn, lote y
	salida a la carpeta Reparados/ sin tocar el original.

	Se pone aqui (y no en main.cpp) porque el Makefile de la GUI
	enlaza todos los .cpp de src/ menos main.o: asi CLI y GUI usan
	exactamente el mismo codigo de reparacion.

	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#ifndef REPAIR_JOB_H
#define REPAIR_JOB_H

#include <string>
#include <utility>
#include <vector>

#include "diagnoser.h"

struct RepairOutcome {
	bool ok = false;
	bool skipped = false;      // ya existia la salida y esta -skip
	bool retried = false;      // se hizo el reintento automatico con -s
	DiagCode code = DiagCode::Ok;
	double recovered_percent = 0;  // 0-100; 100 = sin premature end
	double first_percent = 0;      // % del primer intento (si hubo reintento)
	double retry_percent = 0;      // % del reintento (si lo hubo)
	std::string output_path;       // mejor salida (la que hay que quedarse)
	std::string first_output_path;
	std::string retry_output_path;
	int64_t duration_ms = 0;   // duracion del video reparado (0 si no se sabe)
	uint64_t output_bytes = 0;
	std::string mode_es;       // "normal", "con -s", "reintento automatico con -s -dyn"...
	std::string message_es;    // resumen en espanol para el usuario
	std::string error_es;      // mensaje de error en espanol (si ok == false)
	std::string retry_error_es;
};

// Repara UN archivo con pre-vuelo + reintento automatico.
// - Si el pre-vuelo falla, devuelve ok=false con error_es en
//   espanol y NO llama a Mp4::parseOk() (caso de Angel).
// - Si el primer intento termina en premature end con menos del
//   90% recuperado y el usuario no dio -s, reintenta UNA sola vez
//   con -s -dyn y se queda con el resultado mas largo.
// - La salida va a <carpeta>/Reparados/<nombre>.mp4_fixed.mp4
//   (nombre de Untrunc original), salvo que el usuario haya dado
//   -dst (entonces se respeta -dst).
// - Nunca modifica el archivo original ni el de referencia.
RepairOutcome repairFileWithAutoRetry(const std::string& ref_path,
                                      const std::string& corrupt_path,
                                      bool allow_retry = true);

// Repara VARIOS archivos con la misma referencia (lote, 1.4).
// No se detiene si uno falla: sigue con el siguiente y lo anota
// en el informe.
std::vector<std::pair<std::string, RepairOutcome>> repairBatch(
    const std::string& ref_path,
    const std::vector<std::string>& corrupt_paths,
    bool allow_retry = true);

// Resumen en espanol de un resultado (para CLI y GUI).
std::string repairSummaryEs(const RepairOutcome& outcome, const std::string& corrupt_path);

// Informe final del lote en espanol.
std::string batchSummaryEs(
    const std::vector<std::pair<std::string, RepairOutcome>>& results);

std::string formatDurationEs(int64_t ms);

#endif // REPAIR_JOB_H
