# Repositoryanalyse — Project Verhalenlamp

Analyse van de bestaande QR-museumlamp. Dit rapport beschrijft de aangetroffen
situatie en brengt geen functionele wijziging aan firmware, tools of SD-inhoud.

## Samenvatting

De repository bevat al een bruikbare, volledig offline contentketen: een
gescande QR-ID wordt op de SD-kaart opgezocht in één media-index en speelt,
afhankelijk van het doelbestand, audio, MJPEG-video, een slideshow of een
collectiekaart af. Voor de beoogde jeugd- en volwassenenroutes is de
audio-slideshow de meest passende bestaande vorm. Zij is veel lichter dan
video en combineert een audiotekst met getimede beelden.

Twee doelgroepen vereisen geen nieuwe firmware. Maak per fysieke halte twee
stabiele QR-ID's, bijvoorbeeld `jeugd-oven-01` en `ontdek-oven-01`, met elk
een eigen show of audio. De inhoud achter zo'n ID kan later wisselen zonder
QR-labels opnieuw te drukken.

## 1. Mappenstructuur en belangrijkste bestanden

| Locatie | Rol |
| --- | --- |
| `main/main.c` | Centrale ESP-IDF-firmware: hardware, QR-UART, SD, touch, media-index en afspelen. |
| `main/mp3_player.*` | MP3-decodering en I²S-audio. |
| `main/jpegdec_player.*` | JPEGDEC-adapter voor de MJPEG-weergave. |
| `main/Kconfig.projbuild`, `sdkconfig*` | Instellingen voor scanner, audio, MJPEG-klok, ESP32-S3, PSRAM en USB-MSC. |
| `main/idf_component.yml` | Versiegebonden ESP-IDF-componenten: LVGL, JPEG, MP3 en TinyUSB. |
| `third_party/jpegdec` | Vastgelegde JPEGDEC-submodule voor snellere MJPEG-decodering. |
| `sdcard-example/` | Referentie voor de FAT32-SD: index, media, shows, kaarten, narratie en QR-labels. |
| `tools/build-slideshow.py` | Bouwt getimede shows, indexregel en QR-label uit audio en tijdgestempelde dia's. |
| `tools/build-collection.py` | Bouwt collectiekaarten, eerste teksten, brondata en QR-labels uit de openbare collectiepagina. |
| `tools/generate-narration.py` | Maakt Nederlandse MP3-vertellingen uit bewerkbare tekstbestanden. |
| `tools/convert-video.sh` | Zet bronvideo om naar raw MJPEG 480×272, maximaal 10 fps, plus WAV-audio. |
| `tools/build-qr-sheet.py`, `output/pdf/` | Maken de A4-PDF met QR-labels. |
| `docs/PROJECT_VERHALENLAMP_BRIEF.md` | De ontvangen projectbrief en werkafspraken. |

De vendor-displaydriver is bewust uitzonderd in `.gitignore`: alleen de
benodigde driverbronnen onder `docs/1-Demo/Demo_Arduino/DEMO_MJPEG/` worden
bijgehouden. Nieuwe projectdocumenten onder `docs/` moeten daarom bewust aan
Git worden toegevoegd.

## 2. Huidige mediaflow van QR-scan tot afspelen

```text
QR-scanner (TTL-UART, GPIO17)
        ↓
QR-tekst met CR/LF
        ↓
`media-map.csv` op SD → QR-ID;pad;titel
        ↓
Mediawachtrij in de firmware
        ↓
Bestandsextensie kiest speler
  ├─ .wav / .mp3   → audio
  ├─ .mjpeg/.mjpg  → JPEGDEC-video + optioneel gelijknamige WAV/MP3
  ├─ .csv          → `show.csv`: audio + getimede JPEG-dia's
  └─ .jpg/.jpeg    → collectiekaart + optionele `narration/<QR-ID>.mp3`
```

Tijdens media worden herhaalde scans genegeerd. Een tik stopt media; de
onderste strook wijzigt het volume. Onbekende QR's en bestandfouten keren terug
naar het hoofdscherm. Op het scan-scherm bestaat bovendien een
onderhoudsactie voor USB-SD-toegang of een reset.

## 3. Werking van `media-map.csv`

`sdcard-example/media-map.csv` is de runtime-index. Elke niet-lege,
niet met `#` beginnende regel heeft drie puntkomma-gescheiden velden:

```text
qr-id;relatief-pad-vanaf-de-SD-root;titel-op-het-scherm
```

Voorbeeld voor twee routes bij één halte:

```text
jeugd-oven-01;shows/jeugd-oven-01/show.csv;SPOORZOEKERS: DE OVEN
ontdek-oven-01;shows/ontdek-oven-01/show.csv;ONTDEKKERS: DE OVEN
```

De firmware leest de index bij opstarten in, accepteert maximaal 160 regels,
blokkeert paden met `..` en maakt relatieve paden veilig onder `/sdcard`.
De lookup is een exacte tekstvergelijking. Kies dus korte, stabiele,
scannerveilige ID's; kleine letters, cijfers, `-` en `_` zijn de aanbevolen
conventie. De huidige index bevat 63 regels: bestaande video, zeven shows en
48 collectiekaarten.

## 4. Slideshow-opbouw via `show.csv`

Een show staat onder `shows/<id>/` met één audiobestand, `show.csv` en
baseline-JPEG-dia's van 480×272:

```text
audio;audio.mp3
slide;0;slides/001.jpg
slide;8400;slides/002.jpg
```

De tijden zijn oplopende milliseconden vanaf de audiostart; de eerste dia moet
op 0 ms staan en er zijn maximaal 64 dia's. De onderste 48 pixels zijn de
bedieningsstrook. Bij WAV kan de firmware de totale duur afleiden voor de
voortgangsbalk. MP3-shows spelen goed af, maar hebben nog geen exact berekende
totale duur voor die balk.

`tools/build-slideshow.py` is de gewenste redactionele workflow. Een werkmap
bevat precies één `.wav` of `.mp3` en dia's met namen als
`hh.mm.ss.mmm.jpg`/`.png`. De tool maakt 480×272-JPEG's, `show.csv`,
`timings.csv`, een media-indexregel en een QR-label. Standaard is de naam van
het audiobestand zonder extensie ook de QR-ID; leg die naam dus vast vóór het
drukken.

## 5. Collectiekaarten en narratiegeneratie

`tools/build-collection.py` maakt per object onder andere:

```text
info/gss-001.jpg             480×320 kaart met foto en feiten
narration/gss-001.txt        bewerkbare Nederlandse verteltekst
qr/gss-001.png               QR-label
collection.json              geïmporteerde brongegevens en bron-URL's
collection-media-map.csv     voorgestelde indexregels
```

Er zijn nu 48 `gss-*`-kaarten, teksten, MP3-vertellingen en labels. De optie
`--merge-media-map` voegt de kaarten aan de gewone index toe;
`--narration-only` werkt alleen teksten bij. Een kaart start automatisch
`narration/<QR-ID>.mp3` als dat bestaat; een tik stopt de uitleg en keert terug
naar het scan-scherm.

`tools/generate-narration.py` leest de teksten en schrijft MP3's ernaast. De
lokale sleutel staat in het git-genegeerde `OPENAI_API_KEY.env`. De huidige
standaardstem is `marin` met Nederlandse instructies en snelheid 1,05. De
README vermeldt terecht dat bezoekercommunicatie moet aangeven dat dit een
AI-stem is.

Voor twee doelgroepen per collectieobject kan de kaart worden hergebruikt met
twee QR-ID's, bijvoorbeeld `jeugd-gss-001` en `ontdek-gss-001`, en twee
bijbehorende narratiebestanden. Dit vereist alleen content- en indexwerk.

## 6. Bestaande mogelijkheden voor jeugd en volwassenen

| Behoefte | Bestaande mogelijkheid | Status |
| --- | --- | --- |
| Korte scanbare halte | QR-ID → index → zelfstandig medium | Direct beschikbaar |
| Jeugdtekst met zoekvraag | Korte slideshow met audio en dia's | Direct beschikbaar |
| Langere volwassenenuitleg | Slideshow, MP3 of kaart met narratie | Direct beschikbaar |
| Foto met technische feiten | `info/gss-*.jpg` en collectiekaartgenerator | Direct beschikbaar |
| Echt videofragment | Raw MJPEG 480×272, maximaal 10 fps, synchroon WAV/MP3 | Beschikbaar, maar zwaarder |
| Stabiele fysieke labels | Vrije, korte QR-ID's in de index | Direct beschikbaar |
| Offline beheer | FAT32-SD en USB-MSC-onderhoudsmodus | Direct beschikbaar |
| QR-productie | Slideshow-/collectietools plus A4-PDF-generator | Direct beschikbaar |

## 7. Minimale uitbreidingen, alleen indien later nodig

Geen van deze voorstellen is nu uitgevoerd.

1. **Contentconventie zonder firmware:** leg `jeugd-<halte-id>` en
   `ontdek-<halte-id>` vast met een inhoudelijke haltelijst. Dit dekt het
   grootste deel van de routebehoefte.
2. **SD-validatietool:** een klein Python-script kan vóór kopiëren dubbele
   QR-ID's, ontbrekende bestanden, verkeerde beeldformaten en ongeldige
   `show.csv`-verwijzingen melden. Dit is de waardevolste latere uitbreiding.
3. **Redactionele broncatalogus:** een CSV/JSON met halte, doelgroep, titel,
   locatie, QR-ID en status kan later `media-map.csv` en labels genereren;
   de firmware hoeft die catalogus niet te lezen.
4. **Exacte MP3-showvoortgang:** alleen wenselijk als de balk ook bij MP3 exact
   moet eindigen; dat vraagt een duurmanifest of MP3-duurdetectie.

## 8. Risico's en beheersmaatregelen

| Onderwerp | Risico | Beheersmaatregel |
| --- | --- | --- |
| SD-kopie | Index en bestanden raken uit synchronisatie. | Werk vanuit één bronmap, kopieer in één sessie, werp veilig uit en reset de lamp. |
| QR-ID's | Een hergebruikte ID maakt gedrukte labels misleidend. | ID's nooit voor andere inhoud hergebruiken; wijzig alleen het pad of de inhoud erachter. |
| Beheer | `media-map.csv` is handmatig en heeft geen versie/checksum. | Houd een bronindex in Git bij en voeg later een validatietool toe. |
| JPEG/geheugen | Grote of progressieve JPEG's laden niet betrouwbaar. | Kaart: exact 480×320; show/video: baseline-JPEG 480×272; infobeeld maximaal 512 KiB, MJPEG-frame maximaal 384 KiB. |
| Video | Meer dan 10 fps of hoge JPEG-kwaliteit leidt tot dropped frames. | Gebruik video alleen waar nodig; kies voor verhalen meestal slideshows. |
| Audio | Verkeerd formaat of ontbrekend begeleidend bestand geeft stilte/fout. | Gebruik WAV PCM of MP3; zet bij MJPEG een gelijknamige `.wav`/`.mp3` in dezelfde map. |
| Fouten in de zaal | Een QR of bestand ontbreekt. | Test elke QR op de doel-SD; de firmware keert na fouten terug naar het hoofdscherm. |
| Vrijwilligersonderhoud | Handmatig pad- of firmwarewerk is foutgevoelig. | Werk met werkmappen, naamconventies, generators en een gecontroleerde SD-export. |
| Externe bronnen | Online collectiegegevens kunnen wijzigen. | Bewaar kaarten, teksten en `collection.json` als offline, controleerbare snapshot. |

## 9. Voorstel voor eerste kleine mijlpaal

**Mijlpaal 1: één fysieke halte, twee doelgroepen, volledig offline.**

Maak bij één herkenbaar object of locatie twee shows:

```text
jeugd-<halte-id>     30–45 seconden, eenvoudige opdracht of zoekvraag
ontdek-<halte-id>    60–90 seconden, historische en technische context
```

Gebruik per show één audiotrack en drie tot zes dia's via
`build-slideshow.py`. Voeg twee indexregels en twee QR-labels toe, maar druk
nog geen grote oplage. Test vervolgens op de echte lamp met een kind, een
volwassene en een vrijwilliger.

**Acceptatiecriterium:** beide QR-codes starten zonder netwerk een volledige,
begrijpelijke ervaring, stoppen correct bij aanraken en keren terug naar het
scan-scherm. Beoordeel scanbetrouwbaarheid, geluidsniveau, doorlooptijd en of
de map-/ID-conventie voor vrijwilligers helder genoeg is. Pas na deze pilot is
seriematige productie of een validatietool verstandig.

## Uitvoering van deze opdracht

- **Doel:** de bestaande basis analyseren voor de inhoudelijke museumfase.
- **Geraakte bestanden:** alleen deze analyse en de ontvangen projectbrief.
- **Testwijze:** repositorystructuur, firmwaredispatch, SD-voorbeelden,
  generatorinterfaces en README gecontroleerd.
- **Resultaat:** analyse afgerond; geen firmware, tools of SD-media aangepast.
- **Risico:** de lokale werkmap bevat niet-geversioneerde ontwerp- en
  mediabestanden; die blijven bewust buiten de documentatiecommit.
