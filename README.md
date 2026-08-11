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
- een project-zip met JSON-bronstructuur downloaden.

De interface volgt bewust dezelfde volgorde als het beheerwerk:

1. lokale SD-map kiezen, projectgegevens invullen en mapstructuur aanmaken;
2. video toevoegen of converteren;
3. show met dia's, audio en TTS-tekst toevoegen;
4. afbeelding/datakaart met optionele TTS-tekst toevoegen;
5. items, validatie, `media-map.csv` en QR-codes controleren en daarna de
   projectgegevens naar de gekozen SD-map exporteren.

De video-converter gebruikt ffmpeg.wasm in de browser. Bronvideo's worden dus
niet naar een server geüpload. De vaste QR-lamp preset is 480×272 pixels met
losse audio naast de video. Voor hardwaretests kan de beheerder kiezen uit
10, 15, 20 of 25 fps; 10 fps blijft de aanbevolen veilige instelling voor de
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
  `videos/<id>.mp3` opslaan en de media-mapregel toevoegen.

Browsers geven nooit permanente, stille toegang tot willekeurige lokale
bestanden. De beheerder kiest de map daarom bewust via de knop in de webapp.
Als de browser deze API niet ondersteunt, blijven de ZIP-downloads de fallback.
