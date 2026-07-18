# Inhoud van de microSD-kaart

Formatteer de kaart als FAT32 en kopieer dit bestand naar de root van de kaart.
Maak daarnaast bijvoorbeeld de mappen `audio/` en `video/` aan en plaats daar de media.
Kopieer ook de map `image/` mee naar de root van de kaart. Daarin staat
`image/logo.jpg`, dat op de hoofdpagina van de lamp wordt getoond.

`media-map.csv` is een puntkomma-gescheiden tabel:

```text
inhoud-van-de-QR-code;pad-naar-media;titel-op-het-scherm
museum:blue-vase;audio/blue-vase.wav;BLAUWE VAAS
museum:vertelling;audio/vertelling.mp3;VERTELLING
museum:film;video/film.mjpeg;FILM
gss-001;info/gss-001.jpg;DIEMA DL 8
```

Audio mag WAV/PCM (8- of 16-bit, mono/stereo, 8-48 kHz) of MP3 (MPEG Layer III) zijn. Voor video wordt raw `.mjpeg`/`.mjpg` verwacht: aaneengeschakelde baseline-JPEG-frames, 480×272 (480×320 wordt nog ondersteund voor oude bestanden), zonder AVI/MP4-container. De video staat bovenaan; de onderste 48 pixels zijn de volumebalk. Plaats voor synchroon geluid een PCM-WAV met dezelfde basisnaam naast het videobestand, bijvoorbeeld `video/film.mjpeg` en `video/film.wav`; alleen de MJPEG komt in `media-map.csv`.

Een `.jpg`/`.jpeg`-pad in `media-map.csv` is een offline infokaart van exact 480×320 pixels. De lamp toont deze kaart schermvullend en keert bij tikken terug naar de scanpagina. Genereer de collectiekaarten automatisch met `tools/build-collection.py` uit de repository.
