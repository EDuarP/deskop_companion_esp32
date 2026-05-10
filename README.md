# 🤖 Desktop Companion (ESP32)

Mascota virtual interactiva diseñada para vivir en tu escritorio, proporcionando compañía y reacciones emocionales basadas en la interacción física.

## 🌟 Características principales

El "Bichito" posee un sistema de estados dinámico que reacciona a cómo lo tratas:

- **SISTEMA DE MOOD:** Tiene un nivel de ánimo que decae con el tiempo. Interactuar con él sube su ánimo.
- **ESTADOS EMOCIONALES:**
    - 😊 **HAPPY:** Ojos felices y mejillas sonrojadas.
    - ❤️ **LOVE:** Ojos de corazón y sonidos de ronroneo.
    - 😲 **SURPRISED:** Reacción inmediata a ruidos fuertes (aplausos).
    - 😠 **ANNOYED:** Ojos angulados y color rojo.
    - 😴 **SLEEPY:** Entra en modo sueño tras un tiempo de inactividad.
    - 😢 **SAD:** Ojos tristes y lágrimas animadas.
    - 😡 **RAGE:** Modo furia activado por toques bruscos mientras está triste.
    - 🔫 **FED_UP:** Estado crítico donde el bichito saca un arma y apunta al usuario.

## 🛠️ Hardware Necesario

- **Controlador:** ESP32.
- **Pantalla:** Circular GC9A01 (240x240).
- **Sensores:**
    - Sensor táctil en la cabeza.
    - Sensor táctil en la mejilla.
    - Micrófono analógico.
- **Audio:** Buzzer/Altavoz conectado a GPIO.
- **Energía:** Módulo de batería con divisor de tensión para monitoreo de carga.

## 🚀 Instalación y Uso

1. Clona este repositorio.
2. Instala la librería `LovyanGFX`.
3. Configura los pines según el archivo `.ino` (pines 8, 10, 7, 9, 6 para pantalla; 1, 4, 5 para sensores).
4. Carga el código usando el Arduino IDE o PlatformIO.

## 🎨 Detalles Técnicos
El proyecto utiliza **interpolación lineal (LERP)** para transiciones suaves entre las formas de los ojos y cálculos de tangentes en tiempo real para renderizar corazones matemáticamente precisos.
