# Plantillas de cámara

Una **plantilla** (`*.plantilla`) guarda la cabecera real de un modelo de
cámara: el `ftyp` y el `moov` de un vídeo sano, con los parámetros del
codificador (SPS/PPS), la estructura de pistas y las escalas de tiempo.
**No contiene imagen ni sonido.** Pesa unos KB.

## Por qué sirven

Untrunc solo usa la *estructura* de la referencia, no sus datos de imagen.
Con la plantilla de la misma cámara se recupera lo mismo que con el vídeo
sano. En cambio, una referencia generada con otro codificador (la referencia
sintética de libx264) casi nunca recupera nada, porque sus SPS/PPS y su
estructura no son los de la cámara.

## Crear una

Usa un vídeo **sano y corto** (10-15 s) grabado con la **misma cámara y la
misma configuración** que el dañado: resolución, fps, códec, estabilización
y modo HDR.

```shell
untrunc --crear-plantilla clip_sano.mp4 gopro_hero9_1080p60
```

Se guarda en tu carpeta de usuario:

- Windows: `%APPDATA%\untrunc\plantillas`
- Linux/macOS: `~/.config/untrunc/plantillas`

Al crearla se borran las fechas, el GPS, el número de serie y demás
metadatos personales (`udta`, `meta`, `uuid` y XMP). Esos bloques se
convierten en `free` con el mismo tamaño, para no mover los
desplazamientos internos.

## Usarla

```shell
untrunc --listar-plantillas
untrunc --plantilla gopro_hero9_1080p60 danado.mp4
untrunc --sin-referencia danado.mp4      # prueba las plantillas solo
```

`--sin-referencia` prueba primero las plantillas, hasta 3. Antes descarta
las de otro códec y da preferencia a la de la misma resolución, o a la que
indiques con `--perfil <nombre>`. Se queda con la que más recupere. Solo si
ninguna sirve, recurre a la referencia sintética.

## Esta carpeta

Untrunc también busca plantillas aquí (`profiles/plantillas/`) y en
`plantillas/`, junto al ejecutable. Si quieres compartir la plantilla de
tu cámara, ponle un nombre descriptivo
(`marca_modelo_resolucion_fps[_codec].plantilla`) y comprueba antes que
recupera un vídeo dañado de esa cámara.
