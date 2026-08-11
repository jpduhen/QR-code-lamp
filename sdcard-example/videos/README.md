# Video's

Plaats voorbereide `.mjpeg`- of experimentele `.avi`-bestanden in deze map.
Commit grote videobestanden bij voorkeur niet rechtstreeks in Git.

Voor MJPEG is de aanbevolen resolutie `480×272`, zodat onderin ruimte blijft
voor volume en voortgang. Zet de gewenste FPS als vierde veld in
`media-map.csv`, bijvoorbeeld:

```text
intro;videos/intro.mjpeg;Introfilm;10
```

Plaats audio met exact dezelfde basisnaam naast de video:

```text
videos/intro.mjpeg
videos/intro.mp3
```
