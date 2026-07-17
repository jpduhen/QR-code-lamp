# Inhoud van de microSD-kaart

Formatteer de kaart als FAT32 en kopieer dit bestand naar de root van de kaart.
Maak daarnaast de map `audio/` aan en plaats daar de genoemde WAV-bestanden.

`media-map.csv` is een puntkomma-gescheiden tabel:

```text
inhoud-van-de-QR-code;pad-naar-audio.wav;titel-op-het-scherm
museum:blue-vase;audio/blue-vase.wav;BLAUWE VAAS
```

Audio moet **WAV/PCM**, 8- of 16-bit, mono of stereo, 8-48 kHz zijn. Voor een eerste test is 22.050 Hz, 16-bit mono een goede keuze. MP3/AAC en MJPEG zijn nog niet onderdeel van deze eerste, audio-gerichte firmwareversie.
