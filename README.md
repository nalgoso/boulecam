# BouleCam 🎥

**BouleCam** es un sistema profesional de cámara web virtual y streaming de ultra-baja latencia (< 20 ms por USB, < 50 ms por Wi-Fi) diseñado para convertir dispositivos móviles Android (y tablets) en fuentes de cámara y micrófono de alta definición para Windows, compatible de forma nativa con **OBS Studio, Zoom, Google Meet, Microsoft Teams y Discord**.

Desarrollado para transmisiones en vivo profesionales con soporte multi-cámara independiente y sincronización en tiempo real.

---

## ✨ Características Principales

- 🚀 **Ultra Baja Latencia**:
  - **Modo Cable USB**: < 20 ms mediante túnel ADB directo sin pasar por el router.
  - **Modo Wi-Fi**: < 50 ms con protocolo TCP binario de alto rendimiento (`TCP_NODELAY`), colas elásticas anti-bufferbloat y auto-descubrimiento UDP inteligente.
- 📹 **Cámara Virtual Nativa Unificada (DirectShow + Media Foundation)**:
  - Controlador universal limpio registrado sin duplicaciones: se expone como una única fuente clara **`BouleCam Virtual Camera`** tanto en aplicaciones Win32 clásicas (**OBS Studio, vMix, Zoom, Discord** vía DirectShow) como en aplicaciones modernas de Windows (**Teams, Windows Camera, Chrome WebRTC** vía Media Foundation).
  - Cero desorden (zero clutter): no duplica nombres ni crea múltiples dispositivos fantasma en los selectores de video externos.
- ⚡ **Conmutador de Cuadros en Tiempo Real (60 FPS / 30 FPS)**:
  - Alterna al instante entre 60 FPS (ultra-fluido) y 30 FPS con un simple clic sobre el indicador de FPS en la ventana principal o desde el menú de Ajustes.
  - Negociación directa con el sensor de la cámara del teléfono (`CONTROL_AE_TARGET_FPS_RANGE`) garantizando máxima fluidez de captura.
- 🎛️ **Soporte Multi-Cámara y OBS**:
  - Transmisión dedicada de ultra-baja latencia hacia OBS Studio (`/obs`, `/obs/1`, etc.) vía HTTP Snapshot/MJPEG de alta tasa de cuadros.
  - Configuración persistente individual: cada dispositivo recuerda su propia rotación, lente, linterna, micrófono, balance de blancos y estado de espejo.
- ⏱️ **Compensación Elástica de Deriva de Audio (Clock Drift Compensator)**:
  - Algoritmo de remuestreo elástico en cruce por cero (zero-crossing) en el motor DSP.
  - Mantiene sincronía perfecta de audio y video en transmisiones continuas de larga duración (más de 2 horas) evitando desfases progresivos o chasquidos.
- 🎙️ **Audio de Estudio con Modos Inteligentes**:
  - Captura PCM nativa a 48000 Hz, 16 bits Mono de ultra baja latencia con buffers elásticos de 20 ms.
  - **Modo Micrófono Principal**: Respuesta plana completa (0 dB) ideal para solistas o locución.
  - **Modo Micrófono Ambiente**: Filtro pasa-altos (Low-Cut HPF a 80 Hz) y atenuación balanceada de -3 dB para capturar el sonido ambiente con calidez vocal, eliminando ruidos sordos de climatización y vibraciones de suelo.
  - Control de volumen por ganancia Windows (dB), vúmetro en tiempo real y muteo instantáneo.
- 🔋 **Ahorro de Energía y Atenuación de Pantalla (Dim Screen)**:
  - Reduce el brillo de la pantalla del teléfono al 1% manteniendo la cámara activa, accesible desde el móvil o remotamente desde la PC para evitar calentamiento de batería.
- 📐 **Selector Remoto de Resolución y Orientación Dinámica**:
  - Cambio en caliente entre 720p, 1080p y 4K con adaptación dinámica de tasa de bits desde la aplicación de escritorio.
  - Soporta streaming vertical (1080x1920) y horizontal (1920x1080) adaptándose a la orientación física del móvil en el trípode.
- 🎛️ **Controles Pro Manuales**:
  - Deslizadores en tiempo real para ISO, Compensación de Exposición (EV), Balance de Blancos (Temperatura) y Enfoque Manual / Autofoco continuo.
- 🎨 **Interfaz de Diseño Unificada**:
  - Paleta de diseño profesional (`#006D3B`, `#4ADE80`, `#7A7A7A`, `#1A1A1A`, `#222222`).
  - Toque en pantalla (tap) para ocultar de forma fluida tanto la barra superior como el dock inferior, dejando la vista 100% limpia.

---

## 🏗️ Arquitectura del Sistema

```
[ Teléfono Android ]
   ├── Cámara (Camera2 API / SurfaceTexture / Selector 720p/1080p/4K)
   ├── Micrófono (AudioRecord 48 kHz PCM / Modos Principal y Ambiente)
   ├── Encoder H.264 (Hardware MediaCodec Zero-Copy)
   └── Control de Reposo Inteligente (Smart Standby)
          │
          ▼  (Protocolo BouleCam Binario por USB / Wi-Fi)
[ Daemon C++ Windows: boulecam-desktop.exe ]
   ├── Receptor TCP Multi-Dispositivo con buffers elásticos
   ├── Decodificador Hardware Media Foundation H.264
   ├── Procesador DSP de Audio (Compensador de Deriva + Filtro HPF 80Hz)
   ├── Memoria Compartida Triple-Buffer IPC (DirectShow & Media Foundation)
   └── Servidor HTTP Reactivo (Puerto 8090)
          │
          ├──────────────────────────┐
          ▼                          ▼
[ Aplicación de Escritorio ]     [ OBS Studio / Windows ]
  (Electron GUI, Control Pro,      - Dispositivos DirectShow / Media Foundation
   Selector de Cámaras/Formatos)   - Browser Source: /obs/1, /obs/2
                                   - Audio sincronizado 48 kHz PCM
```

---

## 📦 Descargas y Releases

Los instaladores y ejecutables finales están disponibles en la sección de [Releases de GitHub](https://github.com/nalgoso/boulecam/releases):

- **`BouleCam.exe`**: Aplicación de escritorio portable para Windows 10/11 (64-bit). No requiere instalación previa.
- **`BouleCam.apk`**: Aplicación móvil para Android 8.0+.

---

## 🚀 Guía de Inicio Rápido

### 1. En la Computadora (Windows)
1. Descarga y abre **`BouleCam.exe`**.
2. La aplicación iniciará automáticamente el motor en segundo plano y el monitor de cámaras.

### 2. En el Teléfono (Android)
1. Instala **`BouleCam.apk`** en tu teléfono o tablet y abre la aplicación.
2. **Modo Cable USB (Recomendado)**:
   - Activa la *Depuración por USB* en las opciones de desarrollador de tu teléfono.
   - Conecta el cable a la PC. El selector circular cambiará a **USB** automáticamente.
3. **Modo Wi-Fi**:
   - Toca el selector superior para seleccionar **Wi-Fi**.
   - El teléfono detectará la PC en tu red local automáticamente y comenzará a transmitir.

### 3. Configuración en OBS Studio y Aplicaciones de Windows

Para máxima versatilidad y rendimiento, **BouleCam opera como dispositivos nativos de entrada de Windows y OBS Studio**:

#### 📹 A. Video Nativo (Cámara Web Directa):
1. Ejecuta `register.bat` como Administrador (o `Instalar-Camara-Virtual.bat`).
2. En OBS Studio, en el panel **Fuentes**, haz clic en **`[+]`** y selecciona **Dispositivo de captura de video**.
3. En el menú desplegable de dispositivos, selecciona **`BouleCam Virtual Camera`**.
4. ¡Listo! La cámara transmitirá con aceleración por hardware a 60 FPS con ultra-baja latencia tanto en DirectShow como en Media Foundation.

#### 🎙️ B. Audio Nativo (Micrófono Directo en el Mezclador de OBS):
1. Ejecuta `Instalar-Audio-Virtual.bat` (o acéptalo al correr `register.bat`). Esto instalará el driver de cable de audio virtual (VB-Audio Virtual Cable).
2. Abre `BouleCam.exe` (el servidor receptor detectará y enlazará automáticamente el audio del móvil con el cable virtual).
3. En OBS Studio, en el panel **Fuentes**, haz clic en **`[+]`** y selecciona **Dispositivo de captura de audio**.
4. En el menú desplegable, selecciona **`CABLE Output (VB-Audio Virtual Cable)`**.
5. ¡Listo! El micrófono del teléfono ingresará directamente en el **Mezclador de Audio de OBS**. Podrás:
   - Ver el vúmetro en tiempo real subiendo y bajando según hables al móvil.
   - Ajustar el volumen con el fader en decibelios (dB).
   - Aplicar filtros profesionales nativos de OBS (Supresión de ruido, Compresor y Limitador).

#### 🌐 C. Fuente de Navegador (Alternativa opcional):
- Si deseas integrar la señal como Browser Source, puedes usar la URL `http://127.0.0.1:8090/obs/1` (o `/obs/2` para multi-cámara).

---

## 🛠️ Estructura del Código Fuente

```
boulecam/
├── shared/                     # Estructuras comunes de red e IPC (C/C++)
│   └── include/
│       ├── boulecam_protocol.h # Protocolo binario H.264 y Audio PCM 48 kHz
│       └── boulecam_ipc.h      # Memoria compartida Triple-Buffer IPC
├── desktop-service/            # Motor receptor C++20 para Windows
│   ├── src/
│   │   ├── audio/              # DSP de audio, compensador de deriva y filtros
│   │   ├── decoder/            # Decodificación acelerada por hardware (Media Foundation)
│   │   ├── network/            # TCP_NODELAY, HTTP Bridge y Streaming WAV
│   │   ├── usb/                # ADB reverse tunnel automático
│   │   └── main.cpp            # Orquestador del servicio y gestor de reposo
│   └── CMakeLists.txt
├── desktop-app/                # Interfaz de usuario para Windows (Electron)
│   ├── index.html              # UI de control, selectores y controles manuales
│   ├── renderer.js             # Lógica de sincronización reactiva y almacenamiento local
│   ├── styles.css              # Paleta de colores unificada y responsive
│   └── main.js                 # Integración con el motor C++ y bandeja del sistema
├── mobile/android/             # Aplicación móvil nativa Android (Kotlin)
│   ├── app/src/main/java/com/boulecam/
│   │   ├── audio/              # Captura de audio de ultra baja latencia (AudioRecord)
│   │   ├── camera/             # Pipeline de cámara (Camera2 API y selector de resolución)
│   │   ├── encoder/            # Codificador H.264 por hardware (MediaCodec)
│   │   ├── network/            # Auto-descubrimiento y streaming de video/audio
│   │   └── MainActivity.kt     # UI nativa, controles Pro, reposo y dock flotante
│   └── virtual-camera/         # Controladores DirectShow y Media Foundation
```

---

## 📜 Licencia
Creado 100% con el agente de Antigravity IDE. Siéntanse libres de usarlo gratuitamente.
Desarrollado bajo licencia **MIT**. Desarrollado con ❤️ para la comunidad de streaming y producción en vivo.
