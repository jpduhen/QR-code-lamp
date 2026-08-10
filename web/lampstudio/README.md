# Lamp Studio webapp

Deze map bevat de statische beheerpagina voor Lamp Studio. GitHub Pages kan
de map direct publiceren; er is geen server en geen database nodig.

De webapp is bedoeld voor:

- projectdata invoeren;
- QR-ID's controleren;
- `media-map.csv` voorbereiden;
- QR-codes bekijken;
- korte video's in de browser omzetten naar QR-lamp MJPEG + MP3;
- een project-zip met JSON-bronstructuur downloaden.

De video-converter gebruikt ffmpeg.wasm in de browser. Bronvideo's worden dus
niet naar een server geüpload. De vaste QR-lamp preset is 480×272 pixels met
losse audio naast de video. Voor hardwaretests kan de beheerder kiezen uit
10, 15, 20 of 25 fps; 10 fps blijft de aanbevolen veilige instelling voor de
huidige ESP32-S3 firmware.

Zware of gevoelige stappen blijven bij voorkeur lokaal:

- TTS via OpenAI API;
- lange of grote videoconversies met ffmpeg;
- definitieve SD-export met alle mediabestanden.
