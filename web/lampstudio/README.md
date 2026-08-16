# Lamp Studio webapp

Deze map bevat de statische beheerpagina voor Lamp Studio. GitHub Pages kan
de map direct publiceren; er is geen server en geen database nodig.

De webapp is bedoeld voor:

- projectdata en lokale SD-mapnaam invoeren;
- een lokale SD-map zichtbaar kiezen, bijvoorbeeld `sdcard-gss`;
- bestaande SD-mapstructuren met `media-map.csv` opnieuw inlezen;
- QR-ID's controleren;
- `media-map.csv` voorbereiden;
- QR-codes bekijken;
- een lokale SD-map zoals `sdcard-gss` kiezen en daar `media-map.csv`,
  QR-labels en geconverteerde video's naar schrijven;
- korte video's in de browser omzetten naar QR-lamp MJPEG + MP3;
- shows met dia's, audio en TTS-brontekst voorbereiden;
- afbeeldingen/datakaarten met optionele TTS-brontekst voorbereiden;
- bestaande video's, audio, shows en afbeeldingen vervangen zonder de QR-ID te wijzigen;
- een project-zip met JSON-bronstructuur downloaden.

De interface volgt bewust dezelfde volgorde als het beheerwerk:

1. lokale SD-map kiezen, projectgegevens invullen en mapstructuur aanmaken;
2. video toevoegen of converteren;
3. show met dia's en audio toevoegen, met optionele TTS-brontekst;
4. afbeelding/datakaart met optionele TTS-tekst toevoegen;
5. items, validatie, `media-map.csv` en QR-codes controleren en daarna de
   projectgegevens naar de gekozen SD-map exporteren.

De video-converter gebruikt ffmpeg.wasm in de browser. Bronvideo's worden dus
niet naar een server geüpload. De vaste QR-lamp preset is 480×272 pixels met
losse audio naast de video. Voor hardwaretests kan de beheerder kiezen uit
10, 15, 20 of 25 fps; 15 fps is de aanbevolen standaardinstelling voor de
huidige ESP32-S3 firmware. Lamp Studio schrijft de gekozen FPS als vierde
kolom in `media-map.csv`, zodat de lamp per QR-code de juiste afspeelsnelheid
kan gebruiken.

Zware of gevoelige stappen blijven bij voorkeur lokaal:

- TTS via OpenAI API;
- lange of grote videoconversies met ffmpeg;
- definitieve SD-export met alle mediabestanden.

## Lokale SD-map

Moderne Chromium-browsers ondersteunen de File System Access API. Daarmee kan
Lamp Studio na expliciete toestemming schrijven naar één door de beheerder
gekozen map, bijvoorbeeld de git-genegeerde projectmap `sdcard-gss/`.

De webapp kan:

- de standaardmappen `assets/`, `audio/`, `cards/`, `qr/`, `shows/`, `texts/`
  en `videos/` aanmaken met de knop **Maak mapstructuur**;
- een bestaande `media-map.csv` inlezen met **Lees bestaande items in**;
- waar aanwezig TTS-bronteksten terughalen uit `texts/<qr-id>.txt`;
- na het toevoegen van items `media-map.csv` schrijven;
- printklare QR-labels als PNG in `qr/` schrijven;
- een geconverteerde browser-video als `videos/<id>.mjpeg` met optionele
  `videos/<id>.mp3` opslaan, aan het project toevoegen en de media-mapregel
  bijwerken met één gecombineerde knop.
- per bestaand item via **Vervang** een nieuw mediabestand naar dezelfde
  SD-map schrijven, terwijl de QR-ID gelijk blijft.

## Media vervangen

Lees eerst de bestaande SD-map in, klik daarna in de itemtabel op
**Vervang**. Lamp Studio toont het huidige media-pad en houdt de QR-ID vast.
Dat is belangrijk: zolang de QR-ID gelijk blijft, blijven bestaande geprinte
QR-codes bruikbaar.

Per type werkt vervangen zo:

- `video`: kies een voorbereide `.mjpeg` of experimentele `.avi`. Kies
  optioneel een `.mp3` of `.wav` die naast de video wordt gezet.
- `show`: kies een map of losse bestanden met `show.csv`, audio en `slides/`.
- `image`: kies een nieuwe JPG-kaart of dia.
- `audio`: kies een nieuwe MP3 of WAV.

Als je het doelpad wijzigt, blijft het oude bestand op de SD-map staan totdat
je het handmatig verwijdert. Dat is bewust veilig: Lamp Studio verwijdert geen
oude mediabestanden automatisch.

## Video-export

Na het converteren is de normale hoofdactie **Bewaar video in project +
SD-map**. Als er een lokale SD-map gekozen is, schrijft Lamp Studio meteen de
MJPEG, optionele MP3, `media-map.csv` en het PNG QR-label. Zonder lokale
SD-map wordt de video alleen aan het project toegevoegd. De knop **Download
QR-lamp ZIP** blijft beschikbaar als fallback voor handmatig kopiëren.

Browsers geven nooit permanente, stille toegang tot willekeurige lokale
bestanden. De beheerder kiest de map daarom bewust via de knop in de webapp.
Als de browser deze API niet ondersteunt, blijven de ZIP-downloads de fallback.
