# Cómo contribuir

¡Gracias por querer ayudar! Se aceptan issues y pull requests en **español o
inglés** (*English is welcome too*).

## Formas de ayudar sin programar

- **Cuéntanos qué cámara no se repara bien**, con el formulario
  [«Mi vídeo no se repara»](../../issues/new/choose). Di el modelo, la
  configuración (resolución, fps) y copia el resumen que muestra el programa.
- **Comparte una plantilla de tu cámara.** En la pestaña Asistente, con un
  vídeo **sano** corto (10-15 s) de esa cámara, pulsa «Guardar como plantilla
  de esta cámara». La plantilla guarda solo la cabecera (unos KB): sin imagen,
  sin sonido, sin fecha ni GPS. Con plantillas de más cámaras, el modo sin
  referencia funciona para más gente.
- **Mejora los textos**: si un mensaje no se entiende, dilo o propón otro.

## La regla de privacidad

Este proyecto nunca guarda, copia ni sube vídeos de nadie. Por eso:

- **No adjuntes vídeos** en issues ni pull requests, ni enlaces a vídeos
  personales.
- El código no debe escribir vídeos (completos, recortados, miniaturas ni
  fotogramas) en las carpetas del programa ni de entrenamiento: solo números
  y nombres base. `empaquetar.sh` y `train_onnx.py` lo comprueban y fallan si
  encuentran uno.
- Para las pruebas, usa vídeos sintéticos generados con ffmpeg (ver
  [tests/fixtures/LEEME.md](tests/fixtures/LEEME.md)).

## Para programar

1. Compila y pasa las pruebas siguiendo
   [Compilar desde el código](README-ESPANOL.md#compilar-desde-el-código).
   Antes de enviar un cambio, `./untrunc-tests` debe terminar con
   **0 fallos**.
2. Un cambio por pull request, con una descripción de qué arregla y cómo lo
   probaste (idealmente con un vídeo sintético que reproduzca el caso).
3. Los mensajes que ve el usuario van en español claro, sin jerga.
4. El núcleo heredado de untrunc (`mp4.cpp`, `codec.cpp`, `track.cpp`) se toca
   lo mínimo: si cambias su comportamiento, explica por qué.

El código se publica bajo **GPL-2.0** (ver [COPYING](COPYING)); al contribuir,
aceptas publicar tu aporte con esa licencia.
