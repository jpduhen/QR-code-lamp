# Project Verhalenlamp — samenwerking ChatGPT ↔ Codex

## Opdrachtcontext

Deze repository bevat de werkende QR-museumlamp voor het Gelders Smalspoormuseum. De hardware en kernfunctionaliteit werken al: QR-scannen via UART, media-opzoeking via `media-map.csv`, offline afspelen vanaf SD-kaart, audio, MJPEG-video, slideshows en collectiekaarten.

De volgende fase gaat niet primair over nieuwe hardware, maar over het uitbouwen van een complete offline museumervaring rond de bestaande lamp.

## Projectdoel

Bezoekers ontvangen bij binnenkomst een oude olielamp met ingebouwde QR-scanner, scherm en luidspreker. Door QR-codes op het museumterrein te scannen starten korte audiovisuele verhalen. Alles staat op SD-kaart en werkt zonder wifi.

Er komen minimaal twee inhoudelijke routes:

1. **Spoorzoekers — jeugd**
   - circa 30–45 seconden per halte
   - eenvoudige taal
   - vragen, zoekopdrachten en korte interactieve aanwijzingen

2. **Ontdekkers — volwassenen**
   - circa 60–90 seconden per halte
   - meer historische, sociale en technische context

Later kan een derde verdiepingslaag voor liefhebbers worden toegevoegd.

## Inhoudelijke verhaallijnen

De fysieke route over het museumterrein wordt verdeeld over drie hoofdthema’s:

- ontstaan en geschiedenis van smalspoor;
- leven en werken in de steenfabriek;
- techniek, collectie, locomotieven en rijtuigen.

De presentatie wordt opgeknipt in afzonderlijk scanbare haltes. Per halte kunnen audio, dia’s, foto’s en waar nodig korte videofragmenten worden gebruikt.

## Gewenste samenwerking

### ChatGPT werkt aan

- masterplan en museale verhaallijn;
- jeugd- en volwassenenscripts;
- halteboek en QR-ID’s;
- PowerPoint- en infographicbestanden;
- mediaselectie en timings;
- TTS-teksten en inhoudelijke kwaliteitscontrole.

### Codex werkt in deze repository aan

- analyse en bewaking van de bestaande architectuur;
- technische aansluiting van de content op de huidige firmware en tools;
- robuuste SD-kaartstructuur;
- kleine, gecontroleerde uitbreidingen wanneer die werkelijk nodig zijn;
- tests, documentatie en commits.

## Werkafspraken voor Codex

1. Behandel de bestaande werkende firmware als baseline; geen grote herbouw zonder expliciete opdracht.
2. Werk stap voor stap met kleine, controleerbare wijzigingen.
3. Analyseer eerst, wijzig daarna.
4. Behoud volledige offline werking.
5. Voorkom dat vrijwilligers voor nieuwe inhoud C/C++ hoeven te wijzigen.
6. Gebruik korte, stabiele QR-ID’s; media en teksten moeten later vervangbaar zijn zonder QR-codes opnieuw te drukken.
7. Elke wijziging vermeldt doel, geraakte bestanden, testwijze, resultaat en risico’s.
8. Vermijd cosmetische zijpaden; focus op een robuuste museumrelease.

## Eerste opdracht aan Codex

Voer nog geen codewijzigingen uit.

Maak eerst een analyse van de huidige repository met:

1. mappenstructuur en belangrijkste bestanden;
2. huidige mediaflow van QR-scan tot afspelen;
3. werking van `media-map.csv`;
4. slideshow-opbouw via `show.csv`;
5. collectiekaarten en narratiegeneratie;
6. bestaande mogelijkheden die al voldoen aan de jeugd-/volwassenroute;
7. minimale uitbreidingen die eventueel nog nodig zijn;
8. risico’s voor SD-kaartbeheer, geheugen, foutafhandeling en onderhoud;
9. voorstel voor een kleine, concrete eerste mijlpaal.

Sla het analyserapport op als:

`docs/CODEX_REPOSITORY_ANALYSIS.md`

Maak daarbij nog geen functionele wijzigingen aan firmware, tools of SD-kaartinhoud.

## Belangrijk uitgangspunt

De lamp werkt al. Het doel is nu niet om opnieuw te ontwerpen, maar om de bestaande basis gecontroleerd te gebruiken voor een professionele, onderhoudbare museumervaring.
