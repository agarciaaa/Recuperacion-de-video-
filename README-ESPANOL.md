# Untrunc-mejorado — README en español

> Fork mejorado de [anthwlock/untrunc](https://github.com/anthwlock/untrunc)
> (a su vez fork de [ponchio/untrunc](https://github.com/ponchio/untrunc)).
> Licencia **GNU GPL-2.0** (ver `COPYING`, intacto). Créditos a Federico
> Ponchio, anthwlock y colaboradores. Gratis y de código abierto.

Repara vídeos MP4/MOV/M4V/3GP dañados o truncados, es decir, vídeos que
no se abren porque se interrumpió la grabación y nunca se escribió el
índice (`moov`) final. Untrunc copia la estructura del índice de un
vídeo sano de referencia — mismo dispositivo, misma resolución, mismo
fps, mismo códec — y la aplica a los datos del vídeo dañado.

## Qué trae esta versión mejorada (Fases 1, 2 y 3.2)

- **Diagnóstico previo en español** (Fase 1): antes de reparar, el
  programa comprueba que la referencia tenga `moov`, que los archivos
  no estén intercambiados o sean el mismo, y que ninguno esté vacío.
  Si la referencia no tiene índice, lo dice en español y explica qué
  hacer, en vez del error crudo `no 'moov' atom found`.
- **Reintento automático con `-s -dyn`** (Fase 1): si la reparación se
  corta antes del 90 %, reintenta una vez con `-s -dyn` y se queda con
  el resultado más largo.
- **Lote y no sobrescribir** (Fase 1): repara varios vídeos con la
  misma referencia; la salida va siempre a una carpeta `Reparados/`
  junto al vídeo dañado, sin tocar el original.
- **IA local offline** (Fase 2): detecta SPS/PPS/VPS en los datos,
  escanea límites de frame y elige un perfil de cámara
  (`profiles/profiles.json`: iPhone, Samsung, GoPro, DJI, Sony, Canon,
  OBS y genéricos). Permite reparar **sin referencia** con `--perfil`.
- **IA externa opt-in** (Fase 2): apagada por defecto; solo recibe un
  paquete de diagnóstico en texto de menos de 50 KB, nunca el vídeo.
- **LM Studio — Qwen local** (Fase 2): conector a Qwen 3.5/3.6 en
  `http://localhost:1234`. Qwen solo razona sobre el diagnóstico en
  texto; el vídeo nunca sale de la PC.
- **Pestaña "IA"** en la GUI: cada conexión tiene su check para
  activar/desactivar y sus inputs (endpoint, modelo, clave, tiempo
  de espera), con los inputs en gris cuando la conexión está apagada.
- **Pestaña "Entrenar"** (motor ONNX pequeño): entrena un MLP de
  19.172 parámetros (`.onnx` de 77 KB) con tus propios vídeos buenos
  y corruptos. No se guarda ningún vídeo: solo ventanas de 256 bytes
  con su etiqueta. La base (dataset + modelo) persiste entre sesiones;
  las listas de vídeos no se guardan. Necesita Python 3 + numpy.
- **Varios formatos**: directo MP4, MOV, M4V, 3GP (también HEVC);
  MKV, AVI, WMV, WebM, FLV, MPG y MTS tras convertir a MP4
  (botón "Convertir a MP4" o `--convertir-a-mp4`). La conversión no
  repara por sí sola: después hay que reparar el MP4 resultante.
- **ZIP portable** (Fase 3.2): `untrunc.exe` + `untrunc-gui.exe` +
  DLLs FFmpeg + `profiles/` + `models/onnx_base/` +
  `tools/train_onnx.py` + este README + `LEEME-Primeros-pasos.txt`
  + `COPYING`. Sin instalador y **sin firma de código** (el
  certificado cuesta dinero; ver el aviso de SmartScreen en el
  LEEME). Ver `../phase3/FASE3_EMPAQUETADO.md` (fuera del ZIP)
  con el contenido exacto, los tamaños y cómo se armó el paquete.

## Uso rápido (GUI, recomendado)

1. Doble clic en `untrunc-gui.exe`.
2. Pestaña "Reparar": elige el vídeo dañado y el vídeo de referencia
   (sano, que se reproduzca completo, del mismo dispositivo).
   Sin referencia: marca "Intentar sin referencia" y elige un perfil.
3. Pulsa "Reparar". El resultado queda en `Reparados/`.

## Uso rápido (línea de comandos)

```shell
# Con referencia sana:
./untrunc referencia.mp4 danado.mp4

# Revisión previa (sin reparar nada):
./untrunc --preflight referencia.mp4 danado.mp4

# Lote con la misma referencia:
./untrunc --lote referencia.mp4 danado1.mp4 danado2.mp4

# Plantilla de tu cámara (una vez, con un clip sano corto de la misma cámara):
./untrunc --crear-plantilla clip_sano.mp4 mi_camara_1080p30
./untrunc --plantilla mi_camara_1080p30 danado.mp4

# Rescate sin referencia (prueba primero tus plantillas):
./untrunc --sin-referencia danado.mp4
./untrunc --sin-referencia --perfil generico_h264_1080p30 danado.mp4

# Ver qué texto se enviaría a una IA, sin enviar nada:
./untrunc --solo-diagnostico danado.mp4

# Convertir otro formato a MP4 y luego reparar:
./untrunc --convertir-a-mp4 video.mkv
./untrunc referencia.mp4 video_convertido.mp4

# Entrenar el motor ONNX (necesita Python 3 + numpy):
./untrunc --entrenar-onnx --buenos ref.mp4 --corruptos danado.mp4
./untrunc --resumen-onnx
```

Perfiles disponibles (`profiles/profiles.json`):
`generico_h264_1080p30`, `generico_h264_720p30`,
`generico_h264_360p30`, `iphone_h264_1080p30`, `iphone_hevc_4k30`,
`samsung_h264_1080p30`, `gopro_h264_1080p60`, `gopro_hevc_4k30`,
`dji_h264_4k30`, `sony_xavc_1080p30`, `canon_hevc_4k30`,
`obs_x264_1080p30`.

## Requisitos

- Windows 10/11 de 64 bits (ZIP portable, sin instalación).
- Para reparar con referencia: nada más.
- Para reparar sin referencia, convertir formatos o la vista previa:
  `ffmpeg` (y `ffprobe` para convertir) en el PATH. El ZIP trae las
  DLLs de FFmpeg que enlazan los `.exe`, pero no el `ffmpeg.exe`.
  En MSYS2: `pacman -S mingw-w64-x86_64-ffmpeg`.
- Para entrenar (pestaña "Entrenar"): Python 3 + numpy. En MSYS2:
  `pacman -S mingw-w64-x86_64-python mingw-w64-x86_64-python-numpy`.
- Para Qwen: LM Studio con Qwen 3.5/3.6 cargado y servidor en
  `http://localhost:1234`.

## Privacidad

La reparación, la IA local y el entrenamiento funcionan sin internet.
El vídeo, los fotogramas y el audio nunca se envían a ningún sitio.
Lo único que puede salir de la PC, y solo si tú lo activas, es un
paquete de diagnóstico en texto de menos de 50 KB (a la IA externa
por API, o a LM Studio en `localhost:1234`). La API key nunca va en
la URL ni en los registros. El entrenamiento no guarda ningún vídeo.

## Lo que no puede hacer ninguna versión

Recuperar fotogramas borrados o sobrescritos, reparar un archivo de
0 bytes o sin datos de vídeo, reordenar un archivo fragmentado por
la recuperación, ni "mejorar la calidad" (no se re-codifica el vídeo
reparado).

## Compilar desde el código

```shell
make -j4              # CLI: untrunc
make untrunc-gui      # GUI: untrunc-gui.exe (necesita libui)
make dist-linux       # ZIP de prueba para Linux (ver empaquetar.sh)
make dist-windows     # ZIP para Windows (en MSYS2, tras compilar ambos .exe)
```

Detalles de cada fase (fuera del ZIP, junto al código):
`../analysis/ANALISIS_MEJORADO.md`, `../phase1/FASE1_RESULTADOS.md`,
`../phase2/FASE2_HIBRIDA.md`, `../phase2/LMSTUDIO_QWEN.md`,
`../phase2/GUI_IA_CONEXIONES.md`, `../phase2/ENTRENAR_ONNX.md`,
`../phase2/FORMATOS.md`, `../phase3/FASE3_EMPAQUETADO.md`.
