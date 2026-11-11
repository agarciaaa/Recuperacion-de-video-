# Vídeos de prueba

Vídeos sintéticos de 4 s (320x240, 25 fps, patrón `testsrc2` con ruido),
sin contenido personal. Las pruebas unitarias los dañan en memoria y
comprueban que la reparación vuelve a encontrar el vídeo tras la zona
dañada (`testResincronizacion` en `tests/test_main.cpp`).

| Archivo | Contenido |
|---|---|
| `h264.mp4` | H.264 (x264), sin AUD, como muchos móviles |
| `hevc.mp4` | HEVC (x265, etiqueta `hvc1`) |
| `h264_aac.mp4` | H.264 + audio AAC (tono de 440 Hz) |

Se generaron con FFmpeg 8.0:

```shell
ffmpeg -f lavfi -i "testsrc2=size=320x240:rate=25:duration=4" -vf "noise=alls=12:allf=t" \
  -c:v libx264 -preset veryfast -crf 28 -g 25 -pix_fmt yuv420p \
  -map_metadata -1 -fflags +bitexact -flags:v +bitexact -movflags +faststart h264.mp4

ffmpeg -f lavfi -i "testsrc2=size=320x240:rate=25:duration=4" -vf "noise=alls=12:allf=t" \
  -c:v libx265 -x265-params log-level=error -preset veryfast -crf 30 -g 25 -tag:v hvc1 \
  -pix_fmt yuv420p -map_metadata -1 -fflags +bitexact -flags:v +bitexact -movflags +faststart hevc.mp4

ffmpeg -f lavfi -i "testsrc2=size=320x240:rate=25:duration=4" -f lavfi -i "sine=frequency=440:duration=4" \
  -vf "noise=alls=12:allf=t" -c:v libx264 -preset veryfast -crf 28 -g 25 -pix_fmt yuv420p \
  -c:a aac -b:a 64k -map_metadata -1 -fflags +bitexact -flags:v +bitexact -flags:a +bitexact \
  -movflags +faststart h264_aac.mp4
```
