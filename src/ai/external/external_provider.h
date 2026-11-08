/*
	Untrunc-mejorado (Fase 2 hibrida) - external_provider.h

	IA EXTERNA: cliente generico por API key para un servicio de IA
	que razona sobre texto. Es OPT-IN y esta APAGADA por defecto:
	sin --ia-externa y sin clave, jamas se hace ninguna llamada de red.

	Que se envia: SOLO el DiagnosticPacket (JSON de metadatos, <= 50 KB):
	ftyp, tamanos, SPS en hexadecimal, conteos de NAL, histograma
	de tamanos de slice y el codigo de pre-vuelo. Nombres de
	archivo genericos, nunca rutas reales.

	Que NUNCA se envia: el archivo de video, fotogramas, audio,
	bytes del mdat, metadatos EXIF/GPS, ni la clave en los logs.
	El paquete se valida (validateDiagnosticPacket) antes de enviar
	y se rechaza si supera 50 KB o contiene bytes no imprimibles.

	Como se envia: con el binario `curl` del sistema (Linux y
	Windows 10+ / MSYS2 lo traen), invocandolo con un archivo de
	configuracion temporal con permisos 0600 que lleva el header
	Authorization. La clave NUNCA va en la linea de comandos
	(quedaria visible en `ps`) ni en la URL (se rechaza cualquier
	endpoint que contenga "key=" o "api_key").
	Si no hay curl, el proveedor se declara no disponible y no se
	intenta nada por red.

	No se usa ninguna API key real en este codigo ni en las pruebas:
	los tests usan un endpoint falso en 127.0.0.1:9 (puerto cerrado)
	y --solo-diagnostico, que no hace ninguna llamada.

	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#ifndef AI_EXTERNAL_PROVIDER_H
#define AI_EXTERNAL_PROVIDER_H

#include "../ai_provider.h"

class ExternalProvider : public IAiProvider {
public:
	explicit ExternalProvider(const AiConfig& cfg) : cfg_(cfg) {}
	std::string name() const override { return "externa"; }
	bool isAvailable(std::string& why_not_es) const override;
	AiSuggestion analyze(const DiagnosticPacket& packet,
	                     const AiDiagnosis& diagnosis) override;

	// Construye el paquete de diagnostico de un archivo y, si se
	// pide, lo envia. Con dry_run=true NO se hace ninguna llamada:
	// solo se devuelve el paquete para que el usuario lo revise.
	// (Es lo que usa --solo-diagnostico.)
	DiagnosticPacket buildPacketForFile(const std::string& damaged_path,
	                                    const std::string& ref_path = "");

private:
	AiConfig cfg_;
};

#endif // AI_EXTERNAL_PROVIDER_H
