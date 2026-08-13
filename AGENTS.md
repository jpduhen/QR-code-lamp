# AGENTS.md — Codex-router voor Tijdlamp

Lees vóór wijzigingen altijd eerst de relevante documentatie in `docs/`, met
name `DESIGN_PRINCIPLES.md`, `01_Projectvisie.md`,
`08_ContentStandaard.md` en `09_TechnischOntwerp.md`.

## Werkregels

- Houd software en content strikt gescheiden.
- Behandel een museum als verwisselbaar contentpakket; firmware kent geen
  museumnaam.
- Wijzig bestaande architectuur alleen met duidelijke motivatie.
- Historische feiten moeten controleerbaar zijn via bronnen.
- Content is modulair: hoofdstukken, objecten, media, quizzen en bronnen
  blijven los vervangbaar.
- Werk iteratief en houd commits klein.
- Gebruik Markdown voor documentatie en JSON voor contentdefinities.
- Schrijf uitbreidbare software; vermijd project-specifieke uitzonderingen in
  firmware.
- Voorkom duplicatie in documentatie, mapstructuur en contentformaten.
- Maak geen museumteksten, quizzen of bronclaims zonder expliciete opdracht en
  broncontrole.

## Router

- Visie en doelgroep: `docs/01_Projectvisie.md`
- Karakter van de lamp: `docs/02_Tijdlamp.md`
- Verhaalregels: `docs/03_StoryBible.md`
- Route-sjablonen: `docs/04_Museumroute.md`
- Kennisstructuur: `docs/05_Kennisbank.md`
- Interactievormen: `docs/06_Interactie.md`
- Spelsystemen: `docs/07_Gamification.md`
- Contentmodel: `docs/08_ContentStandaard.md`
- Technische architectuur: `docs/09_TechnischOntwerp.md`
- Productieproces: `docs/10_ContentWorkflow.md`
- Toekomstige toepassingen: `docs/11_Toekomst.md`
