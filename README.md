# SigDash

Nieoficjalny panel dla domowej instalacji Sigenergy na **Waveshare ESP32-S3-Touch-LCD-4.3B**
(Modbus TCP, tylko odczyt; ceny RCE / Energy-Charts; historia na karcie SD; eksport do Excela).

Autor: Marcin · projekt niezwiązany z Sigenergy.

## Build „XIP” (GitHub Actions)

To repozytorium buduje się samo na serwerach GitHuba. Biblioteki Arduino są przebudowywane tak,
żeby program działał z pamięci PSRAM (`CONFIG_SPIRAM_XIP_FROM_PSRAM`) – zapisy do flasha
(Wi-Fi, ustawienia, historia) nie przerywają wtedy obrazu. To rozwiązanie zalecane przez Espressif
na „dryfowanie” ekranów RGB w ESP32-S3.

- Każda zmiana w repozytorium uruchamia build (zakładka **Actions**).
- Gotowe pliki: **Actions → ostatni przebieg → Artifacts → SigDash-webflash**.
- Instalator w przeglądarce (jeśli włączone GitHub Pages): `https://<użytkownik>.github.io/<repozytorium>/`

Ustawienia kompilacji: `platformio.ini`. Wersja dla Arduino IDE jest w osobnej paczce (`SigDash.ino`).
