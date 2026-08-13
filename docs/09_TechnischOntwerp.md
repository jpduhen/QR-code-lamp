# 09 — Technisch ontwerp

Dit document beschrijft de architectuur zonder code.

## QR-workflow

De scanner levert een QR-ID. De engine zoekt die ID op in een lokale index en
start het gekoppelde contentitem.

## Content laden

Alle runtimebestanden staan op SD-kaart. De software gebruikt relatieve paden
en behandelt ontbrekende bestanden als beheerfout, niet als crash.

## Mediaspeler

Ondersteunde vormen zijn audio, afbeeldingen, slideshows en video. Media kiest
de speler via type of bestandsvorm; content bepaalt de inhoud.

## Touchbediening

Touch wordt gebruikt voor stoppen, volume, onderhoudsmodus en later eventueel
quiz- of keuze-interactie.

## Voortgang

Waar mogelijk toont de lamp voortgang tijdens audio, show of video. Voortgang
is informatief en mag de hoofdervaring niet blokkeren.

## Instellingen

Hardware- en systeeminstellingen blijven firmware/configuratie. Museuminhoud
blijft data.

## Opslag

De SD-kaart bevat contentpakketten, media, indexen en QR-labels. Lokale
beheerdata blijft uit Git wanneer die groot of projectspecifiek is.

## Datastructuur

De bestaande `media-map.csv` is de runtime-index. Toekomstige JSON-content kan
hieruit exporteren.

## TODO

- Validatietool voor SD-export.
- Definitief contentpakketmanifest.
