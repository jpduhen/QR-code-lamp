# Lamp Studio

Lamp Studio is de generieke contentlaag boven de QR-lamp. De firmware blijft
klein en voorspelbaar; Lamp Studio maakt uit projectbronnen een SD-export met
`media-map.csv`, QR-labels, shows, afbeeldingen, audio en voorbereid
videomateriaal.

## Projectstructuur

```text
project/
├── project.json
└── items/
    └── ringoven/
        ├── item.json
        ├── story.md
        ├── audio.mp3
        ├── 00.00.00.000.jpg
        └── 00.00.12.000.jpg
```

`project.json` beschrijft de organisatie en de stijl. Elk `item.json`
beschrijft één QR-code.

## Contenttypes

- `show`: audiotrack met tijdgestempelde dia's.
- `image`: één afbeelding of datakaart.
- `audio`: alleen geluid.
- `video`: voorbereid `.mjpeg` of `.avi` met optionele gelijknamige audio.

Video en TTS blijven lokaal: een webpagina mag nooit een geheime OpenAI API-key
bevatten en ffmpeg draait betrouwbaarder op de beheercomputer.

## Gebruik

Installeer de Python-dependencies in de bestaande virtuele omgeving of lokaal:

```sh
python3 -m pip install -r tools/lampstudio/requirements.txt
```

Valideer een project:

```sh
python3 tools/lampstudio/lampstudio.py validate tools/lampstudio/examples/speurtocht-demo
```

Maak een SD-export:

```sh
python3 tools/lampstudio/lampstudio.py export \
  tools/lampstudio/examples/speurtocht-demo \
  --output sd-export \
  --overwrite
```

Maak alleen een QR-sheet van een export:

```sh
python3 tools/lampstudio/lampstudio.py qr-sheet sd-export/qr --output sd-export/qr-labels-a4.pdf
```

## Beheerflow

1. Maak per halte/object één itemmap.
2. Kies een contenttype.
3. Voeg tekst, audio, dia's, afbeelding of voorbereid videobestand toe.
4. Draai `validate`.
5. Draai `export`.
6. Kopieer de export naar de SD-kaart of gebruik de USB-SD modus van de lamp.

