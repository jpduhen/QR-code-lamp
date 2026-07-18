#!/usr/bin/env python3
"""Build offline QR collection cards from smalspoor.nl/materieel.php.

The script deliberately reads only the public collection pages supplied by the
Gelders Smalspoormuseum. It produces assets for the lamp's SD card:

  info/gss-001.jpg       480x320 card with photo and technical data
  qr/gss-001.png         printable QR label
  collection-media-map.csv  rows to append to media-map.csv
  collection.json         source URL and raw imported data for review
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


def make_card(item: CollectionItem, image_data: bytes, destination: Path) -> None:
    canvas = Image.new("RGB", CARD_SIZE, "white")
    draw = ImageDraw.Draw(canvas)
    header_font = font(19, True)
    title_font = font(17, True)
    value_font = font(10)
    footer_font = font(10)

    draw.rectangle((0, 0, 479, 36), fill="#103c6b")
    draw.rectangle((0, 36, 479, 40), fill="#f4c400")
    draw.text((12, 8), "Gelders Smalspoormuseum", fill="white", font=header_font)
    title = display_title(item)
    draw.text((12, 47), f"{item.title} — {title}", fill="#111111", font=title_font)

    photo_area = (12, 78, 216, 284)
    photo = Image.open(io.BytesIO(image_data)).convert("RGB")
    photo = ImageOps.contain(photo, (photo_area[2] - photo_area[0], photo_area[3] - photo_area[1]), Image.Resampling.LANCZOS)
    photo_x = photo_area[0] + ((photo_area[2] - photo_area[0]) - photo.width) // 2
    photo_y = photo_area[1] + ((photo_area[3] - photo_area[1]) - photo.height) // 2
    draw.rectangle(photo_area, fill="#e9edf0", outline="#9aa6ad")
    canvas.paste(photo, (photo_x, photo_y))

    y = 78
    right_x = 228
    for label, value in item.details:
        for line in wrap(draw, f"{label}: {value}", value_font, 238, 2):
            draw.text((right_x, y), line, fill="#181818", font=value_font)
            y += 10
        y += 1
        if y > 282:
            break
    draw.rectangle((0, 298, 479, 319), fill="#103c6b")
    draw.text((12, 303), f"QR-code: {item.code}   •   Tik op het scherm om terug te gaan",
              fill="white", font=footer_font)
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
    args = parser.parse_args()

    try:
        from PIL import __version__ as _pillow_version  # noqa: F401
    except ImportError as error:
        raise SystemExit("Installeer eerst: python3 -m pip install -r tools/requirements-collection.txt") from error

    output = args.output.resolve()
    info_dir = output / "info"
    qr_dir = output / "qr"
    info_dir.mkdir(parents=True, exist_ok=True)
    qr_dir.mkdir(parents=True, exist_ok=True)

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
        make_qr(item, qr_dir / f"{item.code}.png")
        items.append(item)
        print(f"[{number}/{len(entries)}] {item.code}: {display_title(item)}")
        time.sleep(0.15)

    rows = [f"{item.code};info/{item.code}.jpg;{display_title(item)[:55]}" for item in items]
    (output / "collection-media-map.csv").write_text(
        "# Gegenereerd uit smalspoor.nl/materieel.php; voeg toe aan media-map.csv\n" + "\n".join(rows) + "\n",
        encoding="utf-8",
    )
    (output / "collection.json").write_text(json.dumps([asdict(item) for item in items], ensure_ascii=False, indent=2) + "\n",
                                                   encoding="utf-8")
    if args.merge_media_map:
        merge_map(output, rows)
    print(f"Klaar: {len(items)} kaarten in {info_dir} en QR-codes in {qr_dir}")


if __name__ == "__main__":
    main()
