# SD-kaart voorbeeldstructuur

Kopieer de inhoud van deze map naar de root van een FAT32-geformatteerde
microSD-kaart. De lamp leest altijd eerst `media-map.csv`; die tabel koppelt
de gescande QR-tekst aan een bestand op de kaart.

```text
sdcard-root/
├── media-map.csv
├── assets/          # algemene beelden, zoals het logo op het scan-scherm
├── cards/           # 480×320 infokaarten
├── audio/           # MP3/WAV bij kaarten of losse audio-items
├── videos/          # MJPEG/AVI met optionele gelijknamige MP3/WAV
├── shows/           # audio-slideshows met show.csv en slides/
├── texts/           # bewerkbare TTS-bronteksten; niet nodig voor afspelen
└── qr/              # printbare QR-labels
```

## media-map.csv

Elke niet-commentaarregel heeft drie of vier velden:

```text
qr-code;relatief-pad;titel;optionele-fps
```

Voorbeelden:

```text
gss-001;cards/gss-001.jpg;Diema DL 8
Smalspoor-1;shows/smalspoor-01/show.csv;SMALSPOOR 1
klokhuis;videos/klokhuis.mjpeg;Klokhuis introductie;15
```

De QR-code zelf is leidend. Een bestaande QR met inhoud `Smalspoor-1` mag dus
gewoon naar de netter genoemde map `shows/smalspoor-01/` wijzen.

## Kaarten met audio

Een kaart staat in `cards/<qr-code>.jpg`. Als er daarnaast
`audio/<qr-code>.mp3` bestaat, speelt de lamp die automatisch af bij het tonen
van de kaart.

## Video

Plaats een voorbereid `.mjpeg`- of experimenteel `.avi`-bestand in `videos/`.
Geluid staat als gelijknamige `.mp3` of `.wav` ernaast:

```text
videos/voorbeeld.mjpeg
videos/voorbeeld.mp3
```

Raw MJPEG bevat zelf geen betrouwbare FPS-metadata. Zet daarom bij MJPEG-video
desgewenst een vierde veld in `media-map.csv`, bijvoorbeeld `;15`.

## Shows

Een slideshow-map bevat minimaal:

```text
shows/voorbeeld/
├── audio.mp3
├── show.csv
└── slides/
    ├── 001.jpg
    └── 002.jpg
```

`show.csv` gebruikt milliseconden vanaf de start van de audio:

```text
audio;audio.mp3
slide;0;slides/001.jpg
slide;8400;slides/002.jpg
```

## Interactieve hoofdstukdemo

De vertical slice voor hoofdstuk 1 start met QR-ID:

```text
smalspoor-h01-landschap
```

Deze verwijst naar `shows/smalspoor-h01-landschap-demo/show.csv`. Dat bestand
gebruikt de bestaande slideshowregels en voegt daarna quizregels toe:

```text
quiz;Waarom stond de steenfabriek juist hier?
answer;B;Omdat hier goede rivierklei lag;1
```

Firmware zonder quizondersteuning negeert deze extra regels en speelt alleen
de slideshow af. Firmware met quizondersteuning toont na de audio een
touch-quiz, feedback en een cliffhanger naar hoofdstuk 2.

De huidige dia's en `audio.wav` zijn placeholders.
