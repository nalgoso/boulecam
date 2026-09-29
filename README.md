# BouleCam 🎥

**BouleCam** es un sistema profesional de cámara web virtual y streaming de ultra-baja latencia (< 20 ms por USB, < 50 ms por Wi-Fi) diseñado para convertir dispositivos móviles Android (y tablets) en fuentes de cámara y micrófono de alta definición para Windows, compatible de forma nativa con **OBS Studio, Zoom, Google Meet, Microsoft Teams y Discord**.

Desarrollado para transmisiones en vivo profesionales con soporte multi-cámara independiente y sincronización en tiempo real.

---

## ✨ Características Principales

- 🚀 **Ultra Baja Latencia**:
  - **Modo Cable USB**: < 20 ms mediante túnel ADB directo sin pasar por el router.
  - **Modo Wi-Fi**: < 50 ms con protocolo TCP binario de alto rendimiento (`TCP_NODELAY`) y auto-descubrimiento UDP inteligente.
- 📹 **Soporte Multi-Cámara Simultáneo**:
  - Conecta múltiples teléfonos o tablets a la misma PC (cada uno recibe su canal independiente: `Cam 1`, `Cam 2`, etc.).
  - Puertos e instancias dedicadas en OBS Studio (`/obs/1`, `/obs/2`, etc.) para mezclar sin interferencias.
  - Configuración persistente individual: cada dispositivo recuerda su propia rotación, lente, linterna, micrófono, balance de blancos y estado de espejo.
- 🔄 **Sincronización en Vivo con OBS Studio**:
  - URL permanente y limpia (ej. `http://127.0.0.1:8090/obs/1`).
  - Al cambiar el espejado o la rotación de 180° desde la app de Windows, **OBS se actualiza automáticamente en menos de 300 ms** sin recargar la fuente.
- 🎙️ **Audio de Estudio en Tiempo Real**:
  - Captura PCM nativa a 48000 Hz, 16 bits Mono de ultra baja latencia.
  - El audio se emite sincronizado dentro del navegador de OBS y se integra directamente en el **Mezclador de Audio de OBS** (control de volumen, compresión, cancelación de ruido).
- 🎛️ **Controles Pro Manuales**:
  - Deslizadores en tiempo real para ISO, Compensación de Exposición (EV), Balance de Blancos (Temperatura) y Enfoque Manual / Autofoco continuo.
- 🔋 **Atenuación de Pantalla (Dim Screen)**:
  - Modo de ahorro de batería al 1% de brillo que mantiene la cámara y la CPU activas sin suspender el teléfono ni recalentar la pantalla.
  - Se puede activar o desactivar tanto desde el móvil como de forma remota desde la PC.
- 📐 **Cero Deformación y Orientación Dinámica**:
  - Soporta streaming vertical (1080x1920) y horizontal (1920x1080) adaptándose a la orientación física del móvil en el trípode.
- 🎨 **Interfaz de Diseño Unificada**:
  - Paleta de diseño profesional (`#006D3B`, `#4ADE80`, `#7A7A7A`, `#1A1A1A`, `#222222`).
  - Toque en pantalla (tap) para ocultar de forma fluida tanto la barra superior como el dock inferior, dejando la vista 100% limpia.

---

## 🏗️ Arquitectura del Sistema

```
[ Teléfono Android ]
   ├── Cámara (Camera2 API / SurfaceTexture)
   ├── Micrófono (AudioRecord 48 kHz PCM)
   └── Encoder H.264 (Hardware MediaCodec Zero-Copy)
          │
          ▼  (Protocolo BouleCam Binario por USB / Wi-Fi)
[ Daemon C++ Windows: boulecam-desktop.exe ]
   ├── Receptor TCP Multi-Dispositivo
   ├── Decodificador Hardware Media Foundation H.264
   ├── Memoria Compartida Triple-Buffer IPC (Webcam Virtual Windows)
   └── Servidor HTTP Reactivo (Puerto 8090)
          │
          ├──────────────────────────┐
          ▼                          ▼
[ Aplicación de Escritorio ]     [ OBS Studio ]
  (Electron GUI, Control Pro,      (Browser Source: /obs/1, /obs/2)
   Selector de Cámaras)            - Video en Vivo
                                   - Audio 48 kHz PCM
                                   - Control en Mezclador de OBS
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
   - Conecta el cable a la PC. El toggle circular cambiará a **USB** automáticamente.
3. **Modo Wi-Fi**:
   - Toca el toggle superior para seleccionar **Wi-Fi**.
   - El teléfono detectará la PC en tu red local automáticamente y comenzará a transmitir.

### 3. Configuración en OBS Studio (Dispositivos Nativos)

Para evitar problemas de compatibilidad y bloqueos de audio de los navegadores, **BouleCam opera como dispositivos nativos de entrada de Windows y OBS Studio**:

#### 📹 A. Video Nativo (Cámara Web Directa):
1. Ejecuta `register.bat` como Administrador (o `Instalar-Camara-Virtual.bat`).
2. En OBS Studio, en el panel **Fuentes**, haz clic en **`[+]`** y selecciona **Dispositivo de captura de video**.
3. En el menú desplegable de dispositivos, selecciona **`BouleCam Virtual Camera`**.
4. ¡Listo! La cámara transmitirá con aceleración por hardware Media Foundation a 60 FPS con ultra-baja latencia.

#### 🎙️ B. Audio Nativo (Micrófono Directo en el Mezclador de OBS):
1. Ejecuta `Instalar-Audio-Virtual.bat` (o acéptalo al correr `register.bat`). Esto instalará el driver firmado de cable de audio virtual (VB-Audio Virtual Cable).
2. Abre `BouleCam.exe` (el servidor receptor detectará y enlazará automáticamente el audio del móvil con el cable virtual).
3. En OBS Studio, en el panel **Fuentes**, haz clic en **`[+]`** y selecciona **Dispositivo de captura de audio**.
4. En el menú desplegable, selecciona **`CABLE Output (VB-Audio Virtual Cable)`**.
5. ¡Listo! El micrófono del teléfono ingresará directamente en el **Mezclador de Audio de OBS**. Podrás:
   - Ver el vúmetro en tiempo real subiendo y bajando según hables al móvil.
   - Ajustar el volumen con el fader en decibelios (dB).
   - Aplicar filtros profesionales nativos de OBS (Supresión de ruido RNNoise, Puerta de ruido, Compresor y Limitador).

#### 🌐 C. Fuente de Navegador (Alternativa opcional):
- Si deseas integrar la señal como Browser Source, puedes seguir usando la URL `http://127.0.0.1:8090/obs/1` (o `/obs/2` para multi-cámara).

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
│   │   ├── decoder/            # Decodificación acelerada por hardware (Media Foundation)
│   │   ├── network/            # TCP_NODELAY, HTTP Bridge y Streaming WAV
│   │   ├── usb/                # ADB reverse tunnel automático
│   │   └── main.cpp            # Orquestador del servicio
│   └── CMakeLists.txt
├── desktop-app/                # Interfaz de usuario para Windows (Electron)
│   ├── index.html              # UI de control, selectores y controles manuales
│   ├── renderer.js             # Lógica de sincronización reactiva y almacenamiento local
│   ├── styles.css              # Paleta de colores unificada y responsive
│   └── main.js                 # Integración con el motor C++ y bandeja del sistema
├── mobile/android/             # Aplicación móvil nativa Android (Kotlin)
│   ├── app/src/main/java/com/boulecam/
│   │   ├── audio/              # Captura de audio de ultra baja latencia (AudioRecord)
│   │   ├── camera/             # Pipeline de cámara (Camera2 API)
│   │   ├── encoder/            # Codificador H.264 por hardware (MediaCodec)
│   │   ├── network/            # Auto-descubrimiento y streaming de video/audio
│   │   └── MainActivity.kt     # UI nativa, controles Pro y dock flotante
└── virtual-camera/             # Controlador de cámara web virtual de Windows
```

---

## 📜 Licencia
Creado 100% con el agente de Antigravity IDE. Siéntanse libres de usarlo gratuitamente
Desarrollado bajo licencia **MIT**. Desarrollado con ❤️ para la comunidad de streaming y producción en vivo.
