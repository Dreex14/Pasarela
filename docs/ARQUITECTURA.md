# Arquitectura de Pasarela

## Objetivo

Igualar la latencia que ya da OBS con una capturadora HDMI, pero con un programa pequeño y de dos botones: **Solo audio** y **Mostrar vídeo**. No se busca superar a OBS a costa de estabilidad.

## 1. Investigación

### Repositorios de referencia (comprobados en sus archivos, no solo en la descripción)

| Proyecto | Licencia | Qué se verificó | Valoración |
| --- | --- | --- | --- |
| SwitchView | GPL-3.0 | DirectShow + VMR9 o MPC Video Renderer/LAV; WASAPI compartido con cola acotada. Sin modo solo audio. Su README reconoce que no ha medido mejora frente a OBS. | Contrastado pero pesado (filtros DirectShow externos). Licencia incompatible con MIT: no se reutiliza nada. |
| CaptureView (hine) | MIT | `audio_pipeline.cpp`: WASAPI compartido por eventos con `AUTOCONVERTPCM`. Source Reader de Media Foundation; detección de bloqueo a los 2 s. | Enfoque sencillo y sólido; mismo esquema que Pasarela. |
| Console Video Capture | Sin licencia | `audio_passthrough.cpp`: WASAPI por eventos + MMCSS «Pro Audio». El modo solo audio no inicia vídeo ni D3D. | Confirma que un modo solo audio real es viable. Sin licencia: solo lectura. |
| DesktopCamApp | MIT | `AudioEngine.cpp`: `IAudioClient3` con el período mínimo del motor y recorte del anillo al objetivo para que la latencia no crezca. Swap chain flip-model con latencia máxima 1. | Contrastado; Pasarela usa ideas equivalentes (con corrección suave de deriva en vez de solo recortes). |
| CaptureViewer | MIT | DirectShow + Sample Grabber + D3D12, doble búfer en CPU. | Funcional, pero D3D12 no aporta nada para un único cuadro por fotograma. |
| Dayaah Capture | MIT | `main.cpp`: `SetMaximumFrameLatency(1)`, `FLIP_DISCARD`, D3D11 Video Processor; «el fotograma nuevo sustituye al no presentado». | Contrastado; mismo principio de «último fotograma gana». |

### Cómo lo hace OBS (código de obs-studio, GPL-2.0, solo como referencia)

- **Vídeo:** `plugins/win-dshow/win-dshow.cpp` captura con DirectShow (libdshowcapture), decodifica MJPEG/H.264 con FFmpeg (opcionalmente por hardware) y tiene la opción *Buffering* (auto/activado/desactivado). La vista previa se compone en el siguiente tic del lienzo (a los FPS del lienzo) y se presenta.
- **Monitorización de audio:** `libobs/audio-monitoring/win32/wasapi-output.c` usa WASAPI compartido con un búfer de 1 s (`Initialize(..., 10000000, ...)`), un *resampler* y una cola de retardo sincronizada por marcas de tiempo con el vídeo.

Conclusión: para igualar a OBS basta con capturar sin búferes intermedios, mostrar siempre el último fotograma en cuanto llega y reproducir el audio por WASAPI con un colchón pequeño. Pasarela hace exactamente eso con menos capas: no hay lienzo, escenas ni mezclador.

## 2. Elección tecnológica

**C++20 nativo (Win32 + Media Foundation + Direct3D 11 + WASAPI), sin bibliotecas externas.**

- Todas las APIs implicadas son nativas de Windows y de C/C++; cualquier otro lenguaje acabaría llamándolas igual, con una capa más.
- Un único `.exe` estático de ~650 KB, sin runtime que instalar.
- **Media Foundation** en lugar de DirectShow: las capturadoras UVC (como la UGREEN 25173) funcionan con MF de forma nativa, se puede elegir el modo exacto que declara el driver y no hace falta registrar filtros. DirectShow quedaría solo para capturadoras PCIe antiguas sin driver AVStream (no incluido en esta versión).
- **Direct3D 11** y no 12: un cuadro por fotograma no se beneficia de D3D12 y D3D11 es más simple y compatible con cualquier GPU (NVIDIA, AMD, Intel). No se usa ninguna función exclusiva de NVIDIA.
- **WASAPI** compartido por eventos (con `IAudioClient3` cuando el driver lo admite). No se ofrece modo exclusivo: para escuchar una consola mientras juegas no aporta nada apreciable, bloquea la salida para el resto de aplicaciones e impide que Discord capture el sonido.
- **Interfaz Win32 propia**, dibujada con Direct2D por software: la ventana principal no carga la GPU, de modo que «Solo audio» no toca Direct3D.

## 3. Módulos

| Archivo | Responsabilidad |
| --- | --- |
| `devices.*` | Enumerar capturadoras (MF) y endpoints de audio (MMDevice); leer los modos reales; emparejar vídeo y audio de la misma capturadora por `ContainerId` (o por nombre); elección automática del modo; avisos de altas/bajas de audio. |
| `video_capture.*` | Source Reader asíncrono con `MF_LOW_LATENCY`. Modo elegido en la propia fuente. Sin conversores para NV12/YUY2/RGB32; decodificador solo para MJPEG. Detecta huecos por marca de tiempo. |
| `renderer.*` | Hilo propio de Direct3D 11. Buzón de una plaza, conversión YUV→RGB en *shader*, escalado con relación de aspecto, textos de estado y diagnóstico con Direct2D. |
| `audio_monitor.*` | Entrada WASAPI → anillo SPSC sin bloqueos → salida WASAPI. Control de deriva, recortes, cebado, volumen y silencio. |
| `app.*` | Ventanas, bandeja, modos, recuperación ante errores, diagnóstico. |
| `settings.*` | `Pasarela.ini` (UTF-16, local). |
| `ui.*` | Tema oscuro, pintor Direct2D, deslizador y desplegables propios. |
| `log.*` | `Pasarela.log` local. |

## 4. Ruta del vídeo

```
capturadora ─USB─▶ driver UVC ─▶ Media Foundation (Source Reader, 1 petición en vuelo)
   ─▶ buzón de 1 plaza (el nuevo sustituye al no mostrado)
   ─▶ hilo de render: espera al waitable de DXGI (máx. 1 fotograma en cola)
   ─▶ toma el MÁS RECIENTE ─▶ 1 copia CPU→GPU ─▶ YUV→RGB (shader) ─▶ escalado ─▶ Present
```

- **No se acumulan fotogramas viejos:** si llega uno nuevo antes de mostrar el anterior, el anterior se descarta (se cuenta como «descartado por viejo»).
- **Una sola copia:** con capturadoras USB el driver entrega memoria de sistema; subirla a la GPU es inevitable. Medido: 0,25 ms a 1080p NV12.
- **Present:** sin VSync se presenta en cuanto está listo (con *tearing* si el sistema lo permite); con VSync, sincronizado. Swap chain `FLIP_DISCARD` y `SetMaximumFrameLatency(1)`.
- **Capturas:** el fotograma ya convertido a RGB a la resolución nativa (antes de escalarlo a la ventana y de dibujar textos) se copia a una textura de lectura y se codifica como PNG de 24 bits con WIC en un hilo aparte, sin frenar el vídeo.
- **Pantalla completa sin bordes**, nunca exclusiva: misma latencia con flip-model y Discord puede capturar la ventana.
- **Modo automático:** ~60 FPS, la mayor resolución que cabe en la pantalla, NV12 > YUY2 > RGB > MJPEG. Si un modo no arranca (p. ej. 4K60 sin comprimir supera el ancho de banda USB, error `0xC00D3704`) se prueba el siguiente.

## 5. Ruta del audio

```
entrada HDMI de la capturadora (WASAPI, evento) ─▶ float ─▶ anillo SPSC
   ─▶ salida (WASAPI, evento, MMCSS "Pro Audio") ─▶ auriculares
```

- **Colchón objetivo** = paquete de captura + medio período de salida + margen (1/4/12 ms según *Ultra bajo/Bajo/Seguro*) + retardo manual.
- **Deriva de reloj:** entrada y salida son aparatos distintos y sus relojes nunca coinciden. Un corrector proporcional ajusta la velocidad de lectura como máximo ±0,3 % (interpolación lineal, inaudible en la práctica) para mantener el colchón en su objetivo. **Así el retardo no crece en sesiones largas.**
- **Atascos:** si sobran más de 40 ms se descarta lo viejo de golpe (con rampa corta); si el anillo se vacía, se vuelve a cebar hasta el objetivo. Ambos se cuentan en el diagnóstico.
- **Frecuencias distintas** entre entrada y salida: Windows convierte (`AUTOCONVERTPCM`), con algo más de latencia; se indica en el diagnóstico.

## 6. Latencias: qué es cada una y qué se puede medir

| Tramo | Origen | ¿Medible por software? |
| --- | --- | --- |
| Capturadora | Escalado/codificación interna y bus USB | No. Solo con prueba externa. |
| Driver de vídeo | Entrega del búfer a Media Foundation | No de forma fiable (la marca de tiempo de MF no es un reloj de llegada al HDMI). |
| Procesamiento de vídeo | Llegada del fotograma → `Present` | **Sí, medido**: 0,61 ms de media (máx. 1,25 ms) con la UGREEN 25173 a 1080p60. |
| Render/pantalla | `Present` → píxel en el monitor (DWM, *scan-out*, monitor) | No. Depende del monitor y de si hay VSync. |
| Entrada de audio | Período del driver (10 ms en la UGREEN) | El período sí (lo declara el driver); la latencia interna, no. |
| Colchón de audio | Anillo + cola de salida | **Sí**: ~19 ms + ~10 ms en *Bajo*, medidos en la sesión. |
| Salida de audio | Driver/auriculares (los inalámbricos añaden bastante) | No. |
| Discord | Codificación, red y búfer del receptor | No; fuera del control de la app. |

El diagnóstico marca como **[medido]** lo medido y como **«estimada (no medida)»** la suma aproximada de audio. La latencia total de extremo a extremo solo puede medirse con una prueba externa (ver [PRUEBAS.md](PRUEBAS.md)).

## 7. Discord

- **Vídeo:** Discord captura ventanas con las APIs de captura de Windows. Una ventana Direct3D 11 flip-model normal o sin bordes es capturable; una pantalla completa exclusiva o una ventana minimizada no. Pasarela nunca usa pantalla completa exclusiva y sigue presentando aunque la ventana quede tapada.
- **Audio:** Discord no documenta su método. Windows ofrece desde 10 2004 la captura de audio por proceso (*application loopback*), que solo ve lo que el proceso reproduce por el mezclador compartido. Pasarela reproduce el sonido desde su propio proceso en modo compartido, que es lo que necesita esa captura. Por eso Pasarela no ofrece modo exclusivo: el audio siempre pasa por el mezclador. **No se instala ningún dispositivo virtual ni se automatiza Discord.**
- **Eco y bucles:** Discord captura el audio de la aplicación, no tu micrófono ni tus auriculares, así que no se duplica. Si compartes la *pantalla entera* con sonido, Discord captura todo el audio del sistema (incluido el de Pasarela): tampoco se duplica, pero incluye el resto de programas.

## 8. Recuperación ante errores

- **Sin fotogramas durante 2 s:** aviso «Sin señal» en la ventana; a los 4 s se reabre la captura, con espera creciente (2, 4, 8 y 10 s).
- **Capturadora desconectada:** se detecta por el aviso de cambios de dispositivos de Windows; al volver con el mismo identificador, se reconecta sola.
- **Audio perdido** (dispositivo invalidado o desconectado): se reintenta cada 3 s; si la salida es la predeterminada y cambia, se reabre en la nueva.
- **GPU perdida** (`DEVICE_REMOVED`): se recrea el renderizador.
- **Capturadora ocupada o privacidad de Windows:** mensajes concretos que dicen qué hacer.

## 9. Compatibilidad y límites conocidos

- Solo dispositivos visibles por Media Foundation (UVC y la mayoría de capturadoras con driver AVStream). Las que solo funcionan con DirectShow no están soportadas en esta versión.
- Formatos mostrados directamente: NV12, YUY2, RGB32; MJPEG se decodifica en CPU (decodificador de Windows). I420, P010 y RGB24 no se ofrecen.
- HDR/P010 no soportado (se muestra en SDR).
- Una capturadora solo admite un programa de vídeo a la vez (OBS, Discord con cámara, la app del fabricante…).

## 10. «Solo audio» con la capturadora activa (opcional)

Con la UGREEN 25173 y la PS5 se observó un corte breve del audio (un petardeo y casi un segundo de silencio) cada 30-40 minutos **solo** en «Solo audio» y **nunca** con el vídeo abierto, ni en «Mostrar vídeo» ni con OBS. No se encontró la causa (se descartaron la pantalla, los eventos de Windows y los búferes de Pasarela), pero sí que la condición común de los casos sin fallo es que la capturadora está enviando imagen por USB.

El ajuste *Capturadora activa en «Solo audio»* (desactivado por defecto) abre en segundo plano el vídeo de la capturadora con el mismo modo que usaría «Mostrar vídeo», y **descarta cada fotograma** sin copiarlo, convertirlo ni mostrarlo (`DiscardSink`). Es un apaño empírico, no una corrección de la causa:

- Se abre y se cierra solo según el modo: al pasar a «Mostrar vídeo» se cierra (la capturadora admite un lector de vídeo a la vez) y al volver se reabre.
- Si falla o deja de entregar fotogramas se reintenta, probando otro modo de los que declara la capturadora.
- Gasta ancho de banda USB y un poco de procesador, y otros programas (OBS) no pueden usar la capturadora a la vez.
