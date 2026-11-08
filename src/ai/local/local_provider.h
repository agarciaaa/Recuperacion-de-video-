/*
	Untrunc-mejorado (Fase 2 hibrida) - local_provider.h

	IA LOCAL: funciona 100% offline, sin internet y sin API key.
	Usa solo codigo y datos que ya estan en la PC:
	  - detector de SPS/PPS (sps_detector)
	  - escaneador de frames (frame_scanner)
	  - base de perfiles de camaras (profile_db)
	Si el modelo local "no esta presente" (por ejemplo, no hay
	perfil que coincida o no se detecta SPS), degrada con gracia:
	devuelve confianza baja y dice que hay que usar una referencia
	sana clasica. Nunca inventa parametros.

	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#ifndef AI_LOCAL_PROVIDER_H
#define AI_LOCAL_PROVIDER_H

#include "../ai_provider.h"

// Analiza un video danado (o sano) SOLO con medios locales.
// No hace red. No modifica el archivo.
AiDiagnosis analyzeLocally(const std::string& path,
                           const std::string& profile_override = "");

class LocalProvider : public IAiProvider {
public:
	std::string name() const override { return "local"; }
	bool isAvailable(std::string& why_not_es) const override;
	AiSuggestion analyze(const DiagnosticPacket& packet,
	                     const AiDiagnosis& diagnosis) override;
	// Atajo: analiza el archivo y devuelve sugerencia directamente.
	AiSuggestion analyzeFile(const std::string& path,
	                         const std::string& profile_override = "");
};

#endif // AI_LOCAL_PROVIDER_H
