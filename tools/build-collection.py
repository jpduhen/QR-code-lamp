#!/usr/bin/env python3
"""Build offline QR collection cards from smalspoor.nl/materieel.php.

The script deliberately reads only the public collection pages supplied by the
Gelders Smalspoormuseum. It produces assets for the lamp's SD card:

  cards/gss-001.jpg      480x320 card with photo and technical data
  texts/gss-001.txt      editable Dutch narration draft for later TTS
  qr/gss-001.png         printable QR label
  media-map.csv          QR lookup table, optionally merged in-place
  texts/collection.json  source URL and raw imported data for review
"""

from __future__ import annotations

import argparse
import html
import io
import json
import re
import time
import urllib.parse
import urllib.request
from dataclasses import asdict, dataclass
from pathlib import Path

import qrcode
from PIL import Image, ImageDraw, ImageFont, ImageOps


BASE_URL = "https://smalspoor.nl/materieel.php"
CARD_SIZE = (480, 320)
USER_AGENT = "QR-code-lamp collection builder/1.0 (museum offline exhibit)"
ORIGIN_CONTEXTS_PATH = Path(__file__).with_name("origin-contexts.json")


@dataclass
class CollectionItem:
    item_id: int
    code: str
    title: str
    source_url: str
    image_url: str
    details: list[tuple[str, str]]


def fetch(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=30) as response:
        return response.read()


def clean_html(value: str) -> str:
    value = re.sub(r"<[^>]+>", " ", value)
    return " ".join(html.unescape(value).replace("\xa0", " ").split())


def index_items(index_html: str) -> list[tuple[int, str]]:
    found: dict[int, str] = {}
    for item_id, label in re.findall(r'href=["\']materieel\.php\?id=(\d+)["\'][^>]*>(.*?)</a>',
                                     index_html, re.IGNORECASE | re.DOTALL):
        found[int(item_id)] = clean_html(label)
    return sorted(found.items())


def parse_item(item_id: int, fallback_title: str) -> CollectionItem:
    source_url = f"{BASE_URL}?id={item_id}"
    page = fetch(source_url).decode("utf-8", "replace")
    heading = re.search(r"<h1[^>]*>(.*?)</h1>", page, re.IGNORECASE | re.DOTALL)
    # Header/footer contain several logos; only the actual collection photo
    # is stored under the site's `mat/` directory.
    image = re.search(r'<img\s+src=["\'](mat/[^"\']+)["\'][^>]*',
                      page, re.IGNORECASE | re.DOTALL)
    if image is None:
        raise ValueError(f"Geen materieelfoto gevonden voor id={item_id}")
    details = [(clean_html(label), clean_html(value))
               for label, value in re.findall(r"<tr>\s*<td[^>]*>(.*?)</td>\s*<td[^>]*>(.*?)</td>\s*</tr>",
                                              page, re.IGNORECASE | re.DOTALL)]
    if not details:
        raise ValueError(f"Geen gegevenstabel gevonden voor id={item_id}")
    return CollectionItem(
        item_id=item_id,
        code=f"gss-{item_id:03d}",
        title=clean_html(heading.group(1)) if heading else fallback_title,
        source_url=source_url,
        image_url=urllib.parse.urljoin(source_url, html.unescape(image.group(1))),
        details=details,
    )


def font(size: int, bold: bool = False) -> ImageFont.ImageFont:
    names = [
        "/System/Library/Fonts/Supplemental/Arial Bold.ttf" if bold else "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/Supplemental/Helvetica Neue.ttf",
        "/Library/Fonts/Arial.ttf",
    ]
    for name in names:
        if Path(name).exists():
            return ImageFont.truetype(name, size)
    return ImageFont.load_default()


def wrap(draw: ImageDraw.ImageDraw, value: str, text_font: ImageFont.ImageFont,
         width: int, lines: int) -> list[str]:
    words = value.split()
    output: list[str] = []
    current = ""
    for word in words:
        candidate = word if not current else f"{current} {word}"
        if draw.textlength(candidate, font=text_font) <= width:
            current = candidate
        else:
            if current:
                output.append(current)
            current = word
    if current:
        output.append(current)
    if len(output) <= lines:
        return output
    clipped = output[:lines]
    while clipped[-1] and draw.textlength(f"{clipped[-1]}…", font=text_font) > width:
        clipped[-1] = clipped[-1][:-1]
    clipped[-1] += "…"
    return clipped


def display_title(item: CollectionItem) -> str:
    fields = dict(item.details)
    manufacturer = fields.get("Fabrikant", "")
    model = fields.get("Type", "")
    return " ".join(part for part in (manufacturer, model) if part) or item.title


def value(fields: dict[str, str], label: str) -> str:
    return fields.get(label, "").strip()


def load_origin_contexts() -> list[tuple[tuple[str, ...], str]]:
    """Load the small, cited editorial context list used for narrations."""
    try:
        payload = json.loads(ORIGIN_CONTEXTS_PATH.read_text(encoding="utf-8"))
        entries = payload["contexts"]
    except (OSError, json.JSONDecodeError, KeyError, TypeError) as error:
        raise RuntimeError(f"Kan herkomstcontext niet laden: {ORIGIN_CONTEXTS_PATH}") from error

    contexts: list[tuple[tuple[str, ...], str]] = []
    for entry in entries:
        matches = entry.get("matches", [])
        text = entry.get("text", "").strip()
        if not isinstance(matches, list) or not matches or not text:
            raise RuntimeError(f"Ongeldige herkomstcontext in {ORIGIN_CONTEXTS_PATH}")
        contexts.append((tuple(matches), text))
    return contexts


ORIGIN_CONTEXTS = load_origin_contexts()


def origin_context_sentences(origin: str) -> list[str]:
    """Return each relevant context sentence once, in editorial file order."""
    return [text for matches, text in ORIGIN_CONTEXTS if any(match in origin for match in matches)]


def known(value: str) -> bool:
    """Do not turn an unknown catalogue value such as '-' into a sentence."""
    return bool(value and value not in {"-", "?"})


def varied(item: CollectionItem, choices: tuple[str, ...]) -> str:
    """Pick a stable wording per object, so consecutive scans do not all sound alike."""
    return choices[(item.item_id - 1) % len(choices)]


def card_facts(item: CollectionItem) -> list[str]:
    """Select a short, readable set of facts for a 3.5-inch display."""
    fields = dict(item.details)
    facts: list[str] = []
    construction = value(fields, "Bouwjaar")
    serial = value(fields, "Fabrieksnummer")
    if construction or serial:
        facts.append("  |  ".join(part for part in (
            f"Bouwjaar: {construction}" if construction else "",
            f"Fabr.nr.: {serial}" if serial else "",
        ) if part))
    nickname = value(fields, "Bijnaam")
    if nickname:
        facts.append(f"Bijnaam: {nickname}")
    origin = value(fields, "Afkomstig van")
    if origin:
        facts.append(f"Herkomst: {origin}")
    gauge = value(fields, "Spoorbreedte")
    weight = value(fields, "Gewicht")
    if gauge or weight:
        facts.append("  |  ".join(part for part in (
            f"Spoor: {gauge}" if gauge else "",
            f"Gewicht: {weight}" if weight else "",
        ) if part))
    drive = value(fields, "Aandrijving")
    if drive:
        facts.append(f"Motor: {drive}")
    operational = value(fields, "Bedrijfsvaardig")
    since = value(fields, "Bij GSS sinds")
    if operational or since:
        facts.append("  |  ".join(part for part in (
            f"Bedrijfsvaardig: {operational}" if operational else "",
            f"Bij GSS sinds: {since}" if since else "",
        ) if part))
    # Some collection records have a different set of fields. Preserve those
    # gracefully instead of leaving the card nearly empty.
    return facts or [f"{label}: {text}" for label, text in item.details[:6]]


def narration(item: CollectionItem) -> str:
    """Create a factual, varied and youth-friendly first draft for a museum voice."""
    fields = dict(item.details)
    maker = value(fields, "Fabrikant")
    model = value(fields, "Type")
    year = value(fields, "Bouwjaar")
    serial = value(fields, "Fabrieksnummer")
    origin = value(fields, "Afkomstig van")
    gauge = value(fields, "Spoorbreedte")
    weight = value(fields, "Gewicht")
    drive = value(fields, "Aandrijving")
    operational = value(fields, "Bedrijfsvaardig")
    since = value(fields, "Bij GSS sinds")
    nickname = value(fields, "Bijnaam")

    if known(nickname):
        sentences = [varied(item, (
            f"Je kijkt naar {item.title}, ook wel {nickname}.",
            f"Dit is {item.title}. De bijnaam is {nickname}.",
            f"Hier staat {item.title}, beter bekend als {nickname}.",
        ))]
    else:
        sentences = [varied(item, (
            f"Je kijkt naar {item.title}.",
            f"Dit is {item.title}.",
            f"Hier zie je {item.title}.",
            f"Maak kennis met {item.title}.",
        ))]

    if known(maker) and known(model) and known(year):
        sentences.append(varied(item, (
            f"Deze {maker} {model} werd gebouwd in {year}.",
            f"In {year} bouwde {maker} deze {model}.",
            f"Deze stoere {maker} {model} komt uit {year}.",
        )))
    elif known(maker) and known(model):
        sentences.append(varied(item, (
            f"Het is een {model} van fabrikant {maker}.",
            f"De fabrikant is {maker}; het type is {model}.",
            f"{maker} bouwde dit type {model}.",
        )))
    elif known(year):
        sentences.append(varied(item, (
            f"Dit museumstuk dateert uit {year}.",
            f"Het object is gebouwd in {year}.",
            f"De bouwtijd: {year}.",
        )))
    if known(serial):
        sentences.append(varied(item, (
            f"Het fabrieksnummer is {serial}.",
            f"Bij de fabrikant kreeg het nummer {serial}.",
            f"Je herkent dit exemplaar aan fabrieksnummer {serial}.",
        )))
    if known(origin):
        sentences.append(varied(item, (
            f"Voordat het bij het museum terechtkwam, was het afkomstig van {origin}.",
            f"Eerder was dit museumstuk in gebruik bij {origin}.",
            f"De route naar het museum loopt via {origin}.",
        )))
        sentences.extend(origin_context_sentences(origin))
    if known(gauge) and known(weight):
        sentences.append(varied(item, (
            f"Het rijdt op {gauge}-spoor en weegt {weight}.",
            f"Met een spoorbreedte van {gauge} en een gewicht van {weight} is dit geen lichtgewicht.",
            f"Het smalle spoor is {gauge} breed; het gewicht is {weight}.",
        )))
    elif known(gauge):
        sentences.append(varied(item, (
            f"Het is gebouwd voor een spoorbreedte van {gauge}.",
            f"Het rijdt op smal spoor van {gauge} breed.",
            f"De spoorbreedte is {gauge}.",
        )))
    elif known(weight):
        sentences.append(varied(item, (
            f"Het gewicht bedraagt {weight}.",
            f"Dit museumstuk weegt {weight}.",
            f"Op de weegschaal komt het uit op {weight}.",
        )))
    if known(drive):
        sentences.append(varied(item, (
            f"Voor de aandrijving zorgt een {drive}.",
            f"Onder de kap zit een {drive}.",
            f"De techniek aan boord: {drive}.",
        )))
    if known(operational):
        if operational.lower() == "ja":
            sentences.append(varied(item, (
                "Hij is bedrijfsvaardig en kan dus nog rijden.",
                "Goed nieuws: dit museumstuk is nog bedrijfsvaardig.",
                "Dit museumstuk kan nog altijd op eigen kracht rijden.",
            )))
        else:
            sentences.append(varied(item, (
                "Hij rijdt nu niet, maar vertelt nog steeds een sterk verhaal.",
                "Dit museumstuk is nu niet bedrijfsvaardig.",
                "Hij staat nu stil, maar laat goed zien hoe het smalspoor werkte.",
            )))
    if known(since):
        sentences.append(varied(item, (
            f"Sinds {since} hoort het bij de collectie van het Gelders Smalspoormuseum.",
            f"Het Gelders Smalspoormuseum bewaart dit object sinds {since}.",
            f"Sinds {since} kun je dit museumstuk hier bekijken.",
        )))
    return "\n\n".join(sentences) + "\n"


def make_card(item: CollectionItem, image_data: bytes, destination: Path) -> None:
    canvas = Image.new("RGB", CARD_SIZE, "white")
    draw = ImageDraw.Draw(canvas)
    header_font = font(19, True)
    title_font = font(17, True)
    value_font = font(13)
    footer_font = font(11, True)

    draw.rectangle((0, 0, 479, 36), fill="#103c6b")
    draw.rectangle((0, 36, 479, 40), fill="#f4c400")
    draw.text((12, 8), "Gelders Smalspoormuseum", fill="white", font=header_font)
    title = display_title(item)
    draw.text((12, 47), f"{item.title} — {title}", fill="#111111", font=title_font)

    photo_area = (12, 78, 210, 290)
    photo = Image.open(io.BytesIO(image_data)).convert("RGB")
    photo = ImageOps.contain(photo, (photo_area[2] - photo_area[0], photo_area[3] - photo_area[1]), Image.Resampling.LANCZOS)
    photo_x = photo_area[0] + ((photo_area[2] - photo_area[0]) - photo.width) // 2
    photo_y = photo_area[1] + ((photo_area[3] - photo_area[1]) - photo.height) // 2
    draw.rectangle(photo_area, fill="#e9edf0", outline="#9aa6ad")
    canvas.paste(photo, (photo_x, photo_y))

    y = 78
    right_x = 222
    for fact in card_facts(item):
        for line in wrap(draw, fact, value_font, 246, 2):
            draw.text((right_x, y), line, fill="#181818", font=value_font)
            y += 15
        y += 3
        if y > 289:
            break
    draw.rectangle((0, 298, 479, 319), fill="#103c6b")
    draw.text((12, 302), "Tik op het scherm om terug te gaan", fill="white", font=footer_font)
    canvas.save(destination, "JPEG", quality=85, optimize=True, progressive=False)


def make_qr(item: CollectionItem, destination: Path) -> None:
    qr = qrcode.QRCode(version=None, error_correction=qrcode.constants.ERROR_CORRECT_M,
                       box_size=12, border=4)
    qr.add_data(item.code)
    qr.make(fit=True)
    code_image = qr.make_image(fill_color="black", back_color="white").convert("RGB")
    code_image.thumbnail((300, 300), Image.Resampling.NEAREST)
    label = Image.new("RGB", (480, 440), "white")
    draw = ImageDraw.Draw(label)
    draw.rectangle((0, 0, 479, 38), fill="#103c6b")
    draw.text((12, 9), "Gelders Smalspoormuseum", fill="white", font=font(18, True))
    y = 50
    for line in wrap(draw, f"{item.title} — {display_title(item)}", font(16, True), 450, 2):
        draw.text((15, y), line, fill="#111111", font=font(16, True))
        y += 19
    label.paste(code_image, ((480 - code_image.width) // 2, 105))
    draw.text((15, 415), f"Scan met de museumlamp  •  {item.code}", fill="#103c6b", font=font(13))
    label.save(destination, "PNG")


def merge_map(output: Path, rows: list[str]) -> None:
    media_map = output / "media-map.csv"
    existing = media_map.read_text(encoding="utf-8") if media_map.exists() else "# qr-content;relative-media-path;title shown on the display\n"
    non_collection = [line for line in existing.splitlines() if not line.startswith("gss-")]
    media_map.write_text("\n".join(non_collection + rows) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("sdcard-example"),
                        help="SD-card folder (default: sdcard-example)")
    parser.add_argument("--limit", type=int, default=0, help="Import only the first N objects (for testing)")
    parser.add_argument("--merge-media-map", action="store_true",
                        help="Append/update generated gss-* rows in media-map.csv")
    parser.add_argument("--narration-only", action="store_true",
                        help="Werk alleen de vertelteksten bij vanuit het bestaande collection.json")
    args = parser.parse_args()

    try:
        from PIL import __version__ as _pillow_version  # noqa: F401
    except ImportError as error:
        raise SystemExit("Installeer eerst: python3 -m pip install -r tools/requirements-collection.txt") from error

    output = args.output.resolve()
    info_dir = output / "cards"
    narration_dir = output / "texts"
    qr_dir = output / "qr"
    info_dir.mkdir(parents=True, exist_ok=True)
    narration_dir.mkdir(parents=True, exist_ok=True)
    qr_dir.mkdir(parents=True, exist_ok=True)

    if args.narration_only:
        collection_path = narration_dir / "collection.json"
        try:
            saved_items = json.loads(collection_path.read_text(encoding="utf-8"))
            items = [CollectionItem(**item) for item in saved_items]
        except (OSError, json.JSONDecodeError, TypeError) as error:
            raise SystemExit(
                f"Kan {collection_path} niet lezen; voer eerst de volledige collectie-import uit."
            ) from error
        for item in items:
            (narration_dir / f"{item.code}.txt").write_text(narration(item), encoding="utf-8")
        print(f"Klaar: {len(items)} vertelteksten bijgewerkt in {narration_dir}")
        return

    index = fetch(BASE_URL).decode("utf-8", "replace")
    entries = index_items(index)
    if args.limit:
        entries = entries[:args.limit]
    if not entries:
        raise SystemExit("Geen collectie-items gevonden; de bronpagina-indeling is mogelijk gewijzigd.")

    items: list[CollectionItem] = []
    for number, (item_id, label) in enumerate(entries, start=1):
        item = parse_item(item_id, label)
        image_data = fetch(item.image_url)
        make_card(item, image_data, info_dir / f"{item.code}.jpg")
        (narration_dir / f"{item.code}.txt").write_text(narration(item), encoding="utf-8")
        make_qr(item, qr_dir / f"{item.code}.png")
        items.append(item)
        print(f"[{number}/{len(entries)}] {item.code}: {display_title(item)}")
        time.sleep(0.15)

    rows = [f"{item.code};cards/{item.code}.jpg;{display_title(item)[:55]}" for item in items]
    (narration_dir / "collection.json").write_text(
        json.dumps([asdict(item) for item in items], ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    if args.merge_media_map:
        merge_map(output, rows)
    print(f"Klaar: {len(items)} kaarten in {info_dir}, teksten in {narration_dir} en QR-codes in {qr_dir}")


if __name__ == "__main__":
    main()
