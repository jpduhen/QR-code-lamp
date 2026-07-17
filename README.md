# QR-museumlamp — ESP-IDF proof of concept

Dit project maakt van de **JC3248W535 ESP32-S3 3,5-inch (480×320)** een draagbare museumgids. De QR-scanner stuurt de gescande tekst via UART. De firmware zoekt die tekst op in `media-map.csv` op de microSD-kaart, toont de titel op het scherm en speelt het gekoppelde WAV- of MP3-bestand via de ingebouwde NS4168-versterker en speakerconnector af. Raw MJPEG-bestanden worden op het scherm afgespeeld.

De POC ondersteunt PCM-WAV (8/16 bit, mono/stereo, 8–48 kHz), MP3 (MPEG Layer III) en raw MJPEG. Voor video gebruikt de nieuwe converter aaneengeschakelde baseline-JPEG-frames van 480×272; de onderste 48 schermpixels blijven zo vrij voor de volumeknoppen. Oude 480×320-bestanden kunnen nog worden gelezen, maar worden bovenaan bijgesneden. Een `.avi`-container of MP4-bestand werkt niet rechtstreeks.

De grote vendor-documentatiemap wordt niet in de repository opgeslagen. Alleen de drie AXS15231B-driverbronnen die de build nodig heeft, staan onder `docs/1-Demo/Demo_Arduino/DEMO_MJPEG/` in versiebeheer.

## Hardware en bekabeling

| Functie | JC3248W535-aansluiting |
| --- | --- |
| Scherm | AXS15231B via interne QSPI, 480×320 in landschapsstand |
| microSD | interne SDMMC 1-bit: CLK 12, CMD 11, D0 13 |
| Audio | interne I²S naar NS4168: BCLK 42, LRCLK 2, DATA 41 |
| QR-scanner TX → ESP32-S3 RX | **P3 of P4 pin 3 / GPIO17** (standaard; instelbaar) |
| QR-scanner GND | P3 of P4 pin 1 |
| Speaker | aansluiting `P6 / SPEAK` op het bord |

Sluit de scanner als volgt aan:

| QR-scanner | JC3248W535 |
| --- | --- |
| GND | GND |
| TXD (pin 5) | P3 of P4 pin 3 / GPIO17 |
| GND (pin 3) | P3 of P4 pin 1 / GND |
| VCC (pin 6) | P3 of P4 pin 2 / 3V3 |
| RXD (pin 4) | P3 of P4 pin 4 / GPIO18 (status- en configuratiecommando's) |
| USB D+ (pin 1), USB D- (pin 2) | niet aansluiten |

De TXD-ingang van de ESP32-S3 is **niet 5V-tolerant**. Deze scannerdocumentatie specificeert 3,3 V voor zowel VCC als TTL-UART, dus er is geen level-shifter nodig. De firmware verwacht UART-TTL, standaard 9600 8N1, met CR/LF of Enter als suffix. Een USB-HID-only scanner werkt niet rechtstreeks met deze UART-aansluiting.

## SD-kaart

Formatteer de kaart als FAT32. Kopieer de inhoud van [`sdcard-example`](sdcard-example/) naar de root van de kaart en voeg de media toe. Het precieze kaartformaat staat in [sdcard-example/README.md](sdcard-example/README.md).

### Video omzetten

Gebruik [`tools/convert-video.sh`](tools/convert-video.sh) voor gewone MP4/MOV/AVI-bronvideo's:

```sh
./tools/convert-video.sh bronvideo.mp4 video/olielamp
```

Dit maakt `olielamp.mjpeg` (480×272, 10 fps) en een apart `olielamp.wav`-audiospoor. De onderste 48 pixels van het scherm blijven vrij voor `− / VOL / +` tijdens video. Zet alleen de `.mjpeg` in `media-map.csv`. De firmware zoekt automatisch het WAV-bestand met dezelfde basisnaam, start het tegelijk met de video en laat beeld op diezelfde 10-fps-klok lopen. Frames die de ESP32-S3 niet tijdig kan decoderen worden overgeslagen om beeld en geluid synchroon te houden.

### Audio-slideshow voor PowerPoint-uitleg

Voor een presentatie met stilstaande dia's gebruikt de lamp een map met een WAV- of MP3-audiotrack en losse 480×272-JPEG-dia's. De tijdlijn staat in `show.csv`; tijden zijn gehele milliseconden vanaf de start van de audio. Dit is lichter dan MJPEG, terwijl `.mjpeg` voor echte video beschikbaar blijft.

```text
audio;audio.mp3
slide;0;slides/001.jpg
slide;8400;slides/002.jpg
```

Maak eerst per presentatie een werkmap met precies één audiobestand en dia's met de starttijd als `hh.mm.ss.mmm` in de bestandsnaam:

```text
work/gss-001/
├── gss-001-uitleg.mp3
├── 00.00.00.000.png
├── 00.00.08.400.jpg
└── 00.00.17.300.png
```

De tool maakt daaruit de juiste SD-map, `show.csv`, een controleerbare `timings.csv`, de `media-map.csv`-regel en een printklaar QR-label:

```sh
tools/.venv/bin/python tools/build-slideshow.py \
  --input work/gss-001 \
  --sd-root sdcard-example
```

Gebruik een scannerveilige audiobestandsnaam, bijvoorbeeld `gss-001-uitleg.mp3`: die wordt automatisch de mapnaam, QR-inhoud, media-mapcode en QR-labelnaam. Zo wisselt `00.00.08.400.jpg` op 8,4 seconden. De bestanden heten na verwerking `slides/001.jpg`, enzovoort. Tikken stopt de presentatie; de volumebalk blijft onder de dia zichtbaar.

### Collectiekaarten en QR-codes

[`tools/build-collection.py`](tools/build-collection.py) importeert de openbare collectiepagina van het Gelders Smalspoormuseum en genereert per object een 480×320-JPEG-infokaart (foto + technische gegevens) en een afdrukbare QR-code. De lamp werkt daarna volledig offline: scan bijvoorbeeld `gss-001` om de kaart van materieelnummer 1 te tonen; tik om terug te keren.

```sh
python3 -m venv tools/.venv
tools/.venv/bin/pip install -r tools/requirements-collection.txt
tools/.venv/bin/python tools/build-collection.py --output sdcard-example --merge-media-map
```

Hiermee ontstaan `info/gss-*.jpg`, `qr/gss-*.png`, `collection.json` (controleerbare brongegevens) en `collection-media-map.csv`. De optie `--merge-media-map` voegt uitsluitend `gss-*`-regels toe of werkt ze bij; bestaande audio- en videoregels blijven staan. Controleer de gegenereerde kaarten vóór het drukken en kopieer daarna `info/`, `qr/` en `media-map.csv` naar de SD-kaart.

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
