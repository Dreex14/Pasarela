# Terceros

El código de Pasarela es propio (MIT, Dreex). No incluye código copiado de otros proyectos.

## Lo que va dentro del ejecutable

`Pasarela.exe` se enlaza estáticamente con las bibliotecas de ejecución del compilador:

| Componente | Licencia | Enlace |
| --- | --- | --- |
| libc++ / libc++abi / libunwind (LLVM) | Apache 2.0 con excepción LLVM | https://llvm.org/LICENSE.txt |
| Runtime de mingw-w64 | ZPL 2.1 y dominio público (ver su archivo COPYING) | https://www.mingw-w64.org/ |

## Herramientas (no se distribuyen)

| Herramienta | Licencia | Enlace |
| --- | --- | --- |
| llvm-mingw (compilador) | Apache 2.0 con excepción LLVM / ISC | https://github.com/mstorsjo/llvm-mingw |

## Componentes de Windows usados en tiempo de ejecución

Media Foundation, WASAPI, Direct3D 11, DXGI, Direct2D, DirectWrite, Windows Imaging Component (PNG) y las fuentes «Segoe UI» y «Segoe Fluent Icons / Segoe MDL2 Assets». Forman parte de Windows; no se incluyen en el programa.

## Proyectos estudiados como referencia (sin reutilizar su código)

| Proyecto | Licencia comprobada |
| --- | --- |
| [SwitchView](https://github.com/NolanChai/SwitchView) | GPL-3.0 (incompatible con MIT: solo se leyó la documentación) |
| [CaptureView](https://github.com/hine/capture-viewer) | MIT (nombre e iconos reservados) |
| [Console Video Capture](https://github.com/sinabehmanesh/console-video-capture) | Sin licencia declarada (todos los derechos reservados: solo lectura) |
| [DesktopCamApp](https://github.com/KaikoClanworth1/DesktopCamApp) | MIT |
| [CaptureViewer](https://github.com/dippyshere/CaptureViewer) | MIT |
| [Dayaah Capture](https://github.com/dayitaah/Dayaah-Capture) | MIT |
| [OBS Studio](https://github.com/obsproject/obs-studio) | GPL-2.0 (solo como referencia de comportamiento) |
