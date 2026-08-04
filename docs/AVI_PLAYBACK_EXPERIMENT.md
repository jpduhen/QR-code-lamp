# AVI playback experiment

Deze branch onderzoekt of AVI/Cinepak een betere route is dan raw MJPEG voor
de Verhalenlamp op de JC3248W535 ESP32-S3.

## Aanleiding

De gevonden ESP32-S3 AVI-player demos gebruiken geen moderne MP4/H.264
decoding, maar een veel eenvoudiger AVI-container met een licht codecprofiel.
De meest bruikbare variant voor deze hardware is Cinepak (`cvid`): dat is oud,
maar het werk per frame is veel kleiner dan bij JPEG-decoding.

## Keuze voor deze POC

- Beeldformaat blijft exact 480x272 pixels.
- De onderste 48 pixels blijven vrij voor volume en voortgang.
- De firmware accepteert alleen AVI met `cvid`/Cinepak-video.
- De FPS wordt uit de AVI-header gelezen.
- Audio in de AVI-container wordt niet gebruikt.
- Geluid blijft een los `.mp3`- of `.wav`-bestand met dezelfde basisnaam.

Voorbeeld:

```text
/sdcard/mjpeg/ringoven.avi
/sdcard/mjpeg/ringoven.mp3
```

In `media-map.csv` verwijs je alleen naar de `.avi`.

## Converter

Gebruik:

```sh
VIDEO_FPS=10 ./tools/convert-avi-cinepak.sh bronvideo.mp4 sdcard-example/mjpeg/ringoven
```

De tool schrijft:

```text
sdcard-example/mjpeg/ringoven.avi
sdcard-example/mjpeg/ringoven.mp3
```

## Verwachting

Deze aanpak moet vooral de CPU-belasting van beelddecoding verlagen. De QSPI
displaytransfer blijft even groot als bij MJPEG, omdat we nog steeds iedere
frame als 480x320-compositie naar het scherm sturen. Als de FPS nog tegenvalt,
ligt de volgende winst waarschijnlijk in het beperken van schermupdates of
het nog verder optimaliseren van de Cinepak-block renderer.

## Beperkingen

- Geen H.264, MPEG-4, Xvid, DivX of MJPEG-in-AVI.
- Geen embedded AVI-audio.
- Geen automatische fallback naar raw MJPEG.
- Getest formaat is 480x272; andere resoluties geven bewust een foutmelding.

