/*
	Untrunc-mejorado - lmstudio_provider.h

	LM STUDIO (Qwen local): tercer proveedor de IA, 100% en
	localhost. LM Studio expone una API compatible con OpenAI en
	http://localhost:1234/v1/chat/completions. El modelo (Qwen 3.5,
	Qwen 3.6 o cualquier otro cargado) corre en la PC del usuario:
	no hay internet ni API key externa.

	Que es Qwen aqui: un LLM DE TEXTO, no un modelo de vision ni
	un modelo ONNX para frames. Qwen NUNCA ve el video ni
	fotogramas: razona solo sobre el DiagnosticPacket en texto
	(ftyp, tamanos, SPS en hexadecimal, conteos de NAL, log).
	Eso encaja con la privacidad: todo queda en localhost.

	Que se envia: SOLO el DiagnosticPacket (JSON de metadatos,
	<= 50 KB) dentro del prompt de un chat OpenAI-compatible.
	Que NUNCA se envia: el archivo de video, fotogramas, audio,
	bytes del mdat, metadatos EXIF/GPS, ni rutas reales.

	Sin API key necesaria en localhost. Si el usuario pone una
	(porque su LM Studio la pide), va en el header Authorization
	Bearer, nunca en la URL ni en los logs.

	Como se usa el nombre del modelo: Qwen 3.5 y 3.6 no tienen un
	nombre fijo en LM Studio; el identificador exacto es el que
	muestra LM Studio en la pestana Developer / Server al cargar
	el modelo (p. ej. "qwen2.5-7b-instruct" o similar segun la
	version descargada). Por eso el modelo es CONFIGURABLE:
	  - --lmstudio-model <nombre>
	  - UNTRUNC_LMSTUDIO_MODEL o ai.conf (lmstudio_model=...)
	  - si no se configura, se autodetecta con GET /v1/models
	    (primer modelo con "qwen" en el nombre, si lo hay).
	Ver --lmstudio-listar para ver los modelos cargados.

	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#ifndef AI_LMSTUDIO_PROVIDER_H
#define AI_LMSTUDIO_PROVIDER_H

#include <string>
#include <vector>

#include "../ai_provider.h"

class LMStudioProvider : public IAiProvider {
public:
	explicit LMStudioProvider(const AiConfig& cfg) : cfg_(cfg) {}
	std::string name() const override { return "lmstudio"; }
	bool isAvailable(std::string& why_not_es) const override;
	AiSuggestion analyze(const DiagnosticPacket& packet,
	                     const AiDiagnosis& diagnosis) override;

	// GET {endpoint}/v1/models. Devuelve los identificadores de
	// los modelos cargados en LM Studio (vacio si no responde).
	// No lanza: devuelve false y un motivo en espanol.
	bool listModels(std::vector<std::string>& models_out,
	                std::string& why_not_es) const;

	// Resuelve el modelo a usar: el configurado, o el primer
	// "qwen" de la lista, o el primero de la lista.
	bool resolveModel(std::string& model_out,
	                  std::string& why_not_es) const;

	// URL base sin barra final (p. ej. http://localhost:1234).
	std::string baseUrl() const;
	std::string chatUrl() const;
	std::string modelsUrl() const;

private:
	AiConfig cfg_;
};

// Mensaje estandar en espanol cuando LM Studio no responde.
std::string lmstudioNotRunningEs(const std::string& endpoint);

#endif // AI_LMSTUDIO_PROVIDER_H
