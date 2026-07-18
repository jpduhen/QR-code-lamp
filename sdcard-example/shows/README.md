# Audio-slideshow per QR-code

Maak lokaal eerst een werkmap met één audiofile en dia's waarvan de bestandsnaam `hh.mm.ss.mmm` de starttijd aangeeft:

```text
work/gss-001/
├── gss-001-uitleg.mp3
├── 00.00.00.000.png
├── 00.00.08.400.jpg
└── 00.00.17.300.png
```

Gebruik vervolgens:

```sh
tools/.venv/bin/python tools/build-slideshow.py \
  --input work/gss-001 \
  --sd-root sdcard-example
```

De tool maakt automatisch de map `shows/gss-001/`:

```text
shows/gss-001/
├── audio.mp3
├── show.csv
└── slides/
    ├── 001.jpg
    └── 002.jpg
```

`show.csv` heeft steeds drie velden: `slide;starttijd-in-milliseconden;relatief-pad`. `timings.csv` is het leesbare controloverzicht dat ook de oorspronkelijke bestandsnaam toont.

```text
audio;audio.mp3
slide;0;slides/001.jpg
slide;8400;slides/002.jpg
```

De audiobestandsnaam zonder extensie is automatisch de naam van de presentatie-map, de QR-inhoud, de media-mapcode en het QR-label. Gebruik daarom alleen letters, cijfers, `_` en `-`, bijvoorbeeld `gss-001-uitleg.mp3`. De media-map wordt automatisch bijgewerkt met:

```text
gss-001-uitleg;shows/gss-001/show.csv;DIEMA DL 8 - UITLEG
```

De tool zet bronafbeeldingen automatisch om naar 480×272 baseline-JPEGs, maakt `show.csv` en genereert een museumstijl-QR-label in `qr/`.
