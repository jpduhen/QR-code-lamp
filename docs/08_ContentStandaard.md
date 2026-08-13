# 08 — Contentstandaard

Alle museumcontent wordt data-gedreven beschreven. De firmware en tools lezen
content, maar kennen geen specifiek museum.

## Basismetadata

- `id`: stabiele QR- en content-ID.
- `museum`: contentpakketnaam.
- `title`: titel op scherm en label.
- `audience`: doelgroep.
- `language`: taalcode.
- `duration_target_seconds`: gewenste duur.
- `learning_goal`: leerdoel.
- `location`: plek of objectgroep.
- `sources`: bronverwijzingen.

## Media

- Dia's: bij voorkeur 480×272 voor shows.
- Kaarten/afbeeldingen: afgestemd op het schermformaat.
- Audio: MP3 of WAV, lokaal op SD-kaart.
- Video: QR-lampformaat, met losse gelijknamige audio waar nodig.

## Interactie en quiz

Interacties worden als data beschreven, niet als hardcoded firmwarelogica.

## Voorlopige JSON-structuur

```json
{
  "id": "voorbeeld-01",
  "type": "show",
  "title": "Voorbeeldhalte",
  "audience": "jeugd",
  "media": [],
  "interactions": [],
  "sources": []
}
```

## TODO

- Definitief JSON-schema.
- Validatieregels voor verplichte velden.
- Mapping naar `media-map.csv` en SD-export.
