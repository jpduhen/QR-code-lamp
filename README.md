# QR-museumlamp — ESP-IDF proof of concept

Dit project maakt van de **JC3248W535 ESP32-S3 3,5-inch (480×320)** een draagbare museumgids. De QR-scanner stuurt de gescande tekst via UART. De firmware zoekt die tekst op in `media-map.csv` op de microSD-kaart, toont de titel op het scherm en speelt het gekoppelde WAV-bestand via de ingebouwde NS4168-versterker en speakerconnector af.

De POC implementeert audio volledig: PCM-WAV, 8/16 bit, mono of stereo en 8–48 kHz. Het bord heeft PSRAM en de meegeleverde demo bevat een MJPEG-afspeelpad; MJPEG/video is bewust de volgende, afzonderlijke stap zodat QR, SD, scherm en audio eerst betrouwbaar als één systeem getest kunnen worden.

De grote vendor-documentatiemap wordt niet in de repository opgeslagen. Alleen de drie AXS15231B-driverbronnen die de build nodig heeft, staan onder `docs/1-Demo/Demo_Arduino/DEMO_MJPEG/` in versiebeheer.

## Hardware en bekabeling

| Functie | JC3248W535-aansluiting |
| --- | --- |
| Scherm | AXS15231B via interne QSPI, 480×320 in landschapsstand |
| microSD | interne SDMMC 1-bit: CLK 12, CMD 11, D0 13 |
| Audio | interne I²S naar NS4168: BCLK 42, LRCLK 2, DATA 41 |
| QR-scanner TX → ESP32-S3 RX | **P2 pin 2 / GPIO6** (standaard; instelbaar) |
| QR-scanner GND | een GND-pin op P2 |
| Speaker | aansluiting `P6 / SPEAK` op het bord |

Sluit de scanner als volgt aan:

| QR-scanner | JC3248W535 |
| --- | --- |
| GND | GND |
| TXD | P2 pin 2 / GPIO6 |
| Voeding | volgens het scannerlabel, eventueel 5 V |

De TXD-ingang van de ESP32-S3 is **niet 5V-tolerant**. Gebruik dus een level-shifter of spanningsdeler als de scanner 5 V TTL op TXD afgeeft. De firmware verwacht UART-TTL, standaard 9600 8N1, met CR/LF of Enter als suffix. Een USB-HID-only scanner werkt niet rechtstreeks met deze UART-aansluiting.

## SD-kaart

Formatteer de kaart als FAT32. Kopieer de inhoud van [`sdcard-example`](sdcard-example/) naar de root van de kaart en voeg de WAV-bestanden toe. Het precieze kaartformaat staat in [sdcard-example/README.md](sdcard-example/README.md).

## Bouwen en flashen

De firmware is gebouwd en gecontroleerd met ESP-IDF **v6.0.1**, ESP32-S3-target, 16 MB flash en de 8 MB octal-PSRAM-variant die de aangesloten module rapporteert.

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem31101 flash monitor
```

Pas scannerpin, baudrate en volume aan via:

```sh
idf.py menuconfig
# QR museum lamp (JC3248W535)
```

De monitor toont elke ontvangen QR-tekst. Test daarom eerst zonder SD-inhoud of speaker of de scanner exact de codes uit `media-map.csv` doorgeeft.

## Inbouw in de olielamp

- Houd de speakeropening vrij en test het volume in de beoogde behuizing; een metalen of gesloten lampvoet verandert de klank sterk.
- Voed bord, scanner en speaker vanuit een voeding die de gezamenlijke piekstroom aankan.
- Gebruik korte, stabiele QR-ID's, bijvoorbeeld `museum:blue-vase`, zodat audio en beelden op de SD-kaart later kunnen veranderen zonder QR-codes opnieuw te drukken.
