# Pruebas

## Verificado en este PC (14/11/2026)

Equipo: Ryzen 7 5800X, RTX 5070, Windows 11. Capturadora **UGREEN 25173** (USB, UVC) con una Switch 2 encendida. Auriculares CORSAIR HS80 (inalámbricos). OBS abierto a la vez.

| Comprobación | Resultado |
| --- | --- |
| Compilación | Sin errores ni avisos. `Pasarela.exe` de ~650 KB, estático. |
| Detección de la capturadora | ✔ «UGREEN 25173». |
| Modos reales | ✔ 108 modos leídos del driver (hasta 4K60 NV12, 1440p144, 1080p240). No se inventa ninguno. |
| Emparejado de audio automático | ✔ «HDMI (UGREEN 25173)» por `ContainerId`. |
| Modo automático | ✔ Tras corregirlo: 1920×1080 @ 60 NV12 (la pantalla es 1080p). Antes elegía 4K60 NV12, que la capturadora no consigue transmitir (`0xC00D3704`). |
| Vídeo con la consola | ✔ 59,3 FPS recibidos y 59,3 mostrados; 0 descartados por viejos; 4 huecos del driver en el arranque. |
| Tiempo de procesamiento de vídeo | ✔ Medido: copia CPU→GPU 0,25 ms; llegada→Present 0,61 ms de media (máx. 1,25 ms). |
| Audio, 20 s por perfil | ✔ 0 vacíos, 0 recortes, 0 desbordes en *Ultra bajo*, *Bajo* y *Seguro*. Deriva estabilizada en < 15 ppm con el colchón en su objetivo (no crece). |
| Audio al abrir el vídeo | ⚠ 1 vacío puntual justo al abrir la ventana de vídeo (un chasquido único). En la misma sesión, después, ninguno. |
| Consumo «Solo audio» | ✔ CPU 0,02 %, 9,5 MB de RAM privada, 16 hilos, sin driver de GPU cargado. |
| Cierre | ✔ La app se cierra sin dejar procesos. |

Las pruebas de audio se hicieron con el volumen a 0 para no sonar por tus auriculares.

## Pendiente de comprobar por ti

### 1. Oído (2 minutos)
1. Abre Pasarela y pulsa **Solo audio**.
2. Comprueba que oyes la consola por los auriculares, sin chasquidos ni eco.
3. Mueve el volumen y pulsa el altavoz para silenciar.

### 2. Latencia frente a OBS (prueba externa)
El retraso de extremo a extremo no se puede medir por software. Prueba con el móvil:
1. Pon el monitor del *passthrough* y la ventana de vídeo de Pasarela uno al lado del otro (o Pasarela y la vista previa de OBS, por turnos).
2. Graba con el móvil a cámara lenta (240 fps si puede) mientras pulsas un botón que cambie la imagen al instante (abrir un menú).
3. Cuenta los fotogramas entre el cambio en el monitor y el cambio en el PC: cada fotograma a 240 fps son ~4,2 ms.
4. Repite 5 veces con Pasarela y 5 con OBS, con VSync desactivado en Pasarela.

Para el audio: graba con el móvil cerca del altavoz de la TV/monitor y de los auriculares a la vez y golpea algo que suene (o usa un efecto corto del juego); mide la separación entre ambos sonidos en un editor de audio.

### 3. Discord
1. **Mostrar vídeo** → en Discord, *Compartir pantalla* → ventana **«Pasarela — Vídeo»** con sonido.
2. Pide a un amigo que confirme que ve la imagen fluida y oye el juego, sin eco.
3. Si la ve en negro: en Discord, Ajustes › Voz y vídeo, activa la captura con la tecnología más reciente.
4. Con un juego del PC abierto, repite el paso 1: la ventana debe seguir siendo capturable.

### 4. Sesión larga ✔ (según tu uso)
Has usado Pasarela durante mucho rato, en audio y en vídeo, sin ningún problema ni corte audible. El diagnóstico llegó a marcar «Vacíos» muy altos sin que se oyera nada: casi seguro son ratos en que la consola (p. ej. el menú HOME) no emite sonido y la capturadora no manda datos; el medidor no distingue ese caso de un corte real.

### 5. Recuperación
- Desenchufa el USB de la capturadora con el vídeo abierto: debe salir «Capturadora desconectada»; al volver a enchufar, se reconecta sola.
- Apaga la consola con el vídeo abierto: «Sin señal»; al encenderla, vuelve la imagen.
- Apaga los auriculares inalámbricos y vuelve a encenderlos.

### 6. PS5
Desactiva HDCP en la PS5 (*Ajustes › Sistema › HDMI*) y repite la prueba 1 y la 3.
