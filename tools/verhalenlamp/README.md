# Verhalenlamp contentgenerator

`build-story.py` maakt uit één bronmap een complete, offline museumhalte.
De tool bevat geen firmwarelogica. Hij valideert de Verhalenlamp-bronnen en
roept vervolgens [`../build-slideshow.py`](../build-slideshow.py) aan. Zo
blijven JPEG-conversie, timing, QR-opmaak en `media-map.csv` één gedeelde
implementatie.

## Bronmap

```text
work/ringoven/
├── story.md
├── meta.json
├── audio.mp3
├── 00.00.00.000.jpg
├── 00.00.12.300.jpg
└── 00.00.28.500.jpg
```

- `story.md` is de bewerkbare brontekst en mag niet leeg zijn. De eerste
  Markdown-kop is de reservetitel als `meta.json` geen `title` bevat.
- `meta.json` bevat een stabiel `id` en een leesbare `title`.
- `audio.mp3` is de volledige audiotrack van de halte.
- Dia's zijn JPG/PNG-bestanden met starttijd als `hh.mm.ss.mmm`; de eerste
  dia moet op `00.00.00.000` beginnen.

Voorbeeld van `meta.json`:

```json
{
  "id": "ringoven",
  "title": "Ringoven — van klei tot steen",
  "route": "spoorzoekers",
  "location": "Ringoven"
}
```

De `id` moet gelijk zijn aan de bronmapnaam en mag alleen kleine letters,
cijfers, `_` en `-` bevatten. `route` en `location` zijn vrije redactionele
metadata; de firmware leest ze niet.

## Bouwen

Installeer eerst dezelfde Python-afhankelijkheden als voor de bestaande
slideshowtool:

```sh
python3 -m venv tools/.venv
tools/.venv/bin/pip install -r tools/requirements-collection.txt
```

Maak vervolgens de export:

```sh
tools/.venv/bin/python tools/verhalenlamp/build-story.py \
  --input work/ringoven \
  --output sd-export
```

De export bevat:

```text
sd-export/
├── media-map.csv
├── qr/ringoven.png
└── shows/ringoven/
    ├── audio.mp3
    ├── show.csv
    ├── timings.csv
    └── slides/001.jpg, 002.jpg, 003.jpg
```

De media-index krijgt of actualiseert de regel voor `ringoven`. De QR-code
blijft dus stabiel wanneer tekst, audio of dia's later wijzigen. Gebruik
`--dry-run` om alleen te valideren. Een bestaande showmap wordt beschermd;
gebruik na controle `--overwrite` om alleen die halte opnieuw te bouwen.

## Voorbeelden

[`examples/ringoven`](examples/ringoven/) bevat een voorbeeld voor
`story.md` en `meta.json`. Audio en dia's zijn bewust niet als nepmedia
meegeleverd: vervang ze door echte, geredigeerde museumopnames en beelden met
de hierboven beschreven bestandsnamen.
