# MUSEUM-003 — Pilotstructuur Ringoven

Deze map is de redactionele bron voor de eerste echte Verhalenlamp-halte: **De ringoven**. De pilot heeft twee afzonderlijke routes, zodat jeugdige bezoekers en volwassenen elk een passende uitleg krijgen.

## Routes

| QR-ID | Doelgroep | Gewenste speelduur |
| --- | --- | --- |
| `jeugd-ringoven-01` | Jeugd / Spoorzoekers | circa 40 seconden |
| `volwassen-ringoven-01` | Volwassenen / Ontdekkers | circa 75 seconden |

De QR-code bevat per route de bijbehorende QR-ID. `meta.json` en `story.md` zijn de redactionele bronbestanden; er zijn in deze stap nog geen audio- of beeldbestanden opgenomen.

## Voorstel: dia's en beeldwissels

### Jeugd — `jeugd-ringoven-01`

| Tijd | Voorgestelde dia |
| --- | --- |
| `00.00.00.000` | Totaalbeeld ringoven |
| `00.00.09.000` | Ovenopeningen / kamers |
| `00.00.19.000` | Vuurroute of historisch ovenbeeld |
| `00.00.31.000` | Arbeiders / interactieve zoekvraag |

### Volwassenen — `volwassen-ringoven-01`

| Tijd | Voorgestelde dia |
| --- | --- |
| `00.00.00.000` | Totaalbeeld ringoven |
| `00.00.16.000` | Doorsnede of ovenkamers |
| `00.00.34.000` | Vullen, stoken en leeghalen |
| `00.00.54.000` | Smalspoor en intern transport |

## Volgende stap

De feiten en beeldkeuze vereisen nog broncontrole. Ook moet voor ieder beeld de gebruiks- en publicatierechten worden gecontroleerd. Pas daarna worden per route de definitieve audio en tijdgestempelde dia's toegevoegd, bijvoorbeeld `audio.mp3` en `00.00.00.000.jpg`. De bestaande `build-story.py`-tool kan de halte dan exporteren; deze tool verwacht die audio en dia's nadrukkelijk al.
