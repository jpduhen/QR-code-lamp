#!/usr/bin/env python3
"""Build generic QR-lamp projects into offline SD-card exports."""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import qrcode
from PIL import Image, ImageDraw, ImageFont, ImageOps
from reportlab.lib.pagesizes import A4
from reportlab.lib.utils import ImageReader
from reportlab.pdfgen import canvas


REPO_ROOT = Path(__file__).resolve().parents[2]
SLIDESHOW_BUILDER = REPO_ROOT / "tools" / "build-slideshow.py"
IDENTIFIER = re.compile(r"^[a-z0-9][a-z0-9_-]*$")
TIMESTAMPED_SLIDE = re.compile(r"^\d{2}\.\d{2}\.\d{2}\.\d{3}\.(jpe?g|png)$", re.IGNORECASE)
IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png"}
AUDIO_EXTENSIONS = {".mp3", ".wav"}
VIDEO_EXTENSIONS = {".mjpeg", ".mjpg", ".avi"}
VALID_TYPES = {"show", "image", "audio", "video"}
SD_DIR_FOR_TYPE = {
    "image": "info",
    "audio": "audio",
    "video": "mjpeg",
}
CANVAS_IMAGE = (480, 320)
QR_LABEL = (480, 440)
MM = 72 / 25.4


@dataclass
class Project:
    root: Path
    meta: dict[str, Any]
    items: list["Item"]


@dataclass
class Item:
    root: Path
    data: dict[str, Any]

    @property
    def identifier(self) -> str:
        return str(self.data["id"])

    @property
    def title(self) -> str:
        return str(self.data["title"])

    @property
    def kind(self) -> str:
        return str(self.data["type"])

    @property
    def video_fps(self) -> int | None:
        content = self.data.get("content")
        if not isinstance(content, dict) or "fps" not in content:
            return None
        return int(content["fps"])


def read_json(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise ValueError(f"ontbrekend bestand: {path}") from error
    except json.JSONDecodeError as error:
        raise ValueError(f"ongeldige JSON in {path}: {error.msg}") from error
    if not isinstance(data, dict):
        raise ValueError(f"{path} moet een JSON-object bevatten")
    return data


def safe_text(value: object, field: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"veld '{field}' moet een niet-lege tekst zijn")
    text = value.strip()
    if any(character in text for character in ";\r\n"):
        raise ValueError(f"veld '{field}' mag geen puntkomma of regeleinde bevatten")
    return text


def relative_source(item: Item, value: str, field: str) -> Path:
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"{item.identifier}: veld '{field}' ontbreekt")
    path = (item.root / value).resolve()
    if not path.is_file():
        raise ValueError(f"{item.identifier}: bestand ontbreekt: {path}")
    return path


def load_project(root: Path) -> Project:
    meta = read_json(root / "project.json")
    safe_text(meta.get("name"), "name")
    items_root = root / "items"
    if not items_root.is_dir():
        raise ValueError(f"ontbrekende items-map: {items_root}")

    items: list[Item] = []
    for item_dir in sorted(path for path in items_root.iterdir() if path.is_dir()):
        data = read_json(item_dir / "item.json")
        identifier = safe_text(data.get("id", item_dir.name), "id")
        if not IDENTIFIER.fullmatch(identifier):
            raise ValueError(f"{item_dir}: id mag alleen kleine letters, cijfers, _ en - bevatten")
        if item_dir.name != identifier:
            raise ValueError(f"{item_dir}: mapnaam moet gelijk zijn aan id '{identifier}'")
        data["id"] = identifier
        data["title"] = safe_text(data.get("title"), "title")
        kind = safe_text(data.get("type"), "type").lower()
        if kind not in VALID_TYPES:
            raise ValueError(f"{identifier}: type moet een van {sorted(VALID_TYPES)} zijn")
        data["type"] = kind
        items.append(Item(item_dir, data))
    if not items:
        raise ValueError("project bevat nog geen items")
    identifiers = [item.identifier for item in items]
    duplicates = sorted({value for value in identifiers if identifiers.count(value) > 1})
    if duplicates:
        raise ValueError(f"dubbele item-id's: {', '.join(duplicates)}")
    return Project(root, meta, items)


def validate_item(item: Item) -> list[str]:
    warnings: list[str] = []
    content = item.data.get("content")
    if not isinstance(content, dict):
        raise ValueError(f"{item.identifier}: veld 'content' moet een object zijn")

    if item.kind == "show":
        audio_files = [path for path in item.root.iterdir()
                       if path.is_file() and path.suffix.lower() in AUDIO_EXTENSIONS]
        slides = [path for path in item.root.iterdir()
                  if path.is_file() and TIMESTAMPED_SLIDE.fullmatch(path.name)]
        if len(audio_files) != 1:
            raise ValueError(f"{item.identifier}: show verwacht precies één .mp3 of .wav")
        if not slides:
            raise ValueError(f"{item.identifier}: show verwacht dia's als 00.00.00.000.jpg")
        if not any(path.name.startswith("00.00.00.000.") for path in slides):
            raise ValueError(f"{item.identifier}: eerste dia moet starten op 00.00.00.000")
        story = item.root / "story.md"
        if not story.is_file():
            warnings.append(f"{item.identifier}: story.md ontbreekt; handig voor TTS en redactie")
    elif item.kind == "image":
        source = relative_source(item, str(content.get("source", "")), "content.source")
        if source.suffix.lower() not in IMAGE_EXTENSIONS:
            raise ValueError(f"{item.identifier}: image bron moet jpg/png zijn")
    elif item.kind == "audio":
        source = relative_source(item, str(content.get("source", "")), "content.source")
        if source.suffix.lower() not in AUDIO_EXTENSIONS:
            raise ValueError(f"{item.identifier}: audio bron moet mp3/wav zijn")
    elif item.kind == "video":
        source = relative_source(item, str(content.get("source", "")), "content.source")
        if source.suffix.lower() not in VIDEO_EXTENSIONS:
            raise ValueError(f"{item.identifier}: video bron moet mjpeg/mjpg/avi zijn")
        if "fps" in content:
            try:
                fps = int(content["fps"])
            except (TypeError, ValueError) as error:
                raise ValueError(f"{item.identifier}: content.fps moet een getal zijn") from error
            if fps < 1 or fps > 30:
                raise ValueError(f"{item.identifier}: content.fps moet tussen 1 en 30 liggen")
        companion = source.with_suffix(".mp3")
        if not companion.exists() and not source.with_suffix(".wav").exists():
            warnings.append(f"{item.identifier}: geen gelijknamige .mp3 of .wav naast video gevonden")
    return warnings


def validate_project(project: Project) -> list[str]:
    warnings: list[str] = []
    for item in project.items:
        warnings.extend(validate_item(item))
    return warnings


def font(size: int, bold: bool = False) -> ImageFont.ImageFont:
    candidates = [
        "/System/Library/Fonts/Supplemental/Arial Bold.ttf" if bold else "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/Supplemental/Helvetica Neue.ttf",
        "/Library/Fonts/Arial.ttf",
    ]
    for candidate in candidates:
        if Path(candidate).exists():
            return ImageFont.truetype(candidate, size)
    return ImageFont.load_default()


def wrap(draw: ImageDraw.ImageDraw, text: str, text_font: ImageFont.ImageFont,
         width: int, max_lines: int) -> list[str]:
    lines: list[str] = []
    current = ""
    for word in text.split():
        candidate = word if not current else f"{current} {word}"
        if draw.textlength(candidate, font=text_font) <= width:
            current = candidate
        else:
            if current:
                lines.append(current)
            current = word
    if current:
        lines.append(current)
    if len(lines) <= max_lines:
        return lines
    lines = lines[:max_lines]
    lines[-1] = lines[-1].rstrip(" .") + "..."
    return lines


def make_qr_label(project: Project, item: Item, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    qr = qrcode.QRCode(error_correction=qrcode.constants.ERROR_CORRECT_M, box_size=12, border=4)
    qr.add_data(item.identifier)
    qr.make(fit=True)
    code_image = qr.make_image(fill_color="black", back_color="white").convert("RGB")
    code_image.thumbnail((300, 300), Image.Resampling.NEAREST)

    label = Image.new("RGB", QR_LABEL, "white")
    draw = ImageDraw.Draw(label)
    color = str(project.meta.get("theme", {}).get("primary", "#103c6b"))
    organization = str(project.meta.get("organization", project.meta.get("name", "QR-lamp")))
    draw.rectangle((0, 0, QR_LABEL[0] - 1, 38), fill=color)
    draw.text((12, 9), organization[:38], fill="white", font=font(18, True))
    y = 50
    for line in wrap(draw, item.title, font(17, True), 450, 2):
        draw.text((15, y), line, fill="#111111", font=font(17, True))
        y += 21
    label.paste(code_image, ((QR_LABEL[0] - code_image.width) // 2, 108))
    draw.text((15, 416), f"Scan met de QR-lamp - {item.identifier}", fill=color, font=font(13))
    label.save(destination, "PNG")


def image_card(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    image = Image.open(source).convert("RGB")
    image = ImageOps.contain(image, CANVAS_IMAGE, Image.Resampling.LANCZOS)
    canvas = Image.new("RGB", CANVAS_IMAGE, "white")
    canvas.paste(image, ((CANVAS_IMAGE[0] - image.width) // 2, (CANVAS_IMAGE[1] - image.height) // 2))
    canvas.save(destination, "JPEG", quality=86, optimize=True, progressive=False)


def update_media_map(rows: list[str], item: Item, relative_path: str) -> None:
    suffix = ""
    if item.kind == "video" and Path(relative_path).suffix.lower() in {".mjpeg", ".mjpg"}:
        fps = item.video_fps
        if fps is not None:
            suffix = f";{fps}"
    rows.append(f"{item.identifier};{relative_path};{item.title}{suffix}")


def copy_media(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def export_show(project_root: Path, output_root: Path, item: Item) -> str:
    show_dir = output_root / "shows" / item.identifier
    command = [
        sys.executable, str(SLIDESHOW_BUILDER),
        "--input", str(item.root),
        "--sd-root", str(output_root),
        "--output", str(show_dir),
        "--code", item.identifier,
        "--title", item.title,
        "--media-map", str(output_root / ".lampstudio-media-map.tmp"),
        "--qr-output", str(output_root / "qr"),
    ]
    subprocess.run(command, check=True)
    temp_map = output_root / ".lampstudio-media-map.tmp"
    if temp_map.exists():
        temp_map.unlink()
    return f"shows/{item.identifier}/show.csv"


def export_item(project: Project, output_root: Path, item: Item) -> str:
    if item.kind == "show":
        return export_show(project.root, output_root, item)

    content = item.data["content"]
    source = relative_source(item, str(content.get("source", "")), "content.source")
    target_dir = SD_DIR_FOR_TYPE[item.kind]
    target_suffix = source.suffix.lower()
    target = output_root / target_dir / f"{item.identifier}{target_suffix}"
    if item.kind == "image":
        image_card(source, target.with_suffix(".jpg"))
        return f"{target_dir}/{item.identifier}.jpg"
    copy_media(source, target)
    if item.kind == "video":
        for suffix in (".mp3", ".wav"):
            companion = source.with_suffix(suffix)
            if companion.exists():
                copy_media(companion, output_root / target_dir / f"{item.identifier}{suffix}")
    return f"{target_dir}/{item.identifier}{target_suffix}"


def create_qr_sheet(source: Path, output: Path) -> int:
    labels = sorted(source.glob("*.png"), key=lambda path: path.stem.lower())
    if not labels:
        raise ValueError(f"geen QR-labels gevonden in {source}")
    output.parent.mkdir(parents=True, exist_ok=True)
    page_width, page_height = A4
    margin = 7 * MM
    gutter = 1.5 * MM
    columns = 3
    rows = 7
    cell_width = (page_width - 2 * margin - (columns - 1) * gutter) / columns
    cell_height = (page_height - 2 * margin - (rows - 1) * gutter) / rows
    pdf = canvas.Canvas(str(output), pagesize=A4, pageCompression=1)
    pdf.setTitle("QR-lamp labels")
    for index, label in enumerate(labels):
        slot = index % (columns * rows)
        if slot == 0 and index:
            pdf.showPage()
        row, column = divmod(slot, columns)
        cell_x = margin + column * (cell_width + gutter)
        cell_y = page_height - margin - (row + 1) * cell_height - row * gutter
        image = ImageReader(str(label))
        image_width, image_height = image.getSize()
        scale = min(cell_width / image_width, cell_height / image_height)
        draw_width = image_width * scale
        draw_height = image_height * scale
        pdf.drawImage(image, cell_x + (cell_width - draw_width) / 2,
                      cell_y + (cell_height - draw_height) / 2,
                      draw_width, draw_height, mask="auto")
        pdf.setStrokeColorRGB(0.75, 0.75, 0.75)
        pdf.setLineWidth(0.25)
        pdf.rect(cell_x, cell_y, cell_width, cell_height, stroke=1, fill=0)
    pdf.save()
    return len(labels)


def command_validate(args: argparse.Namespace) -> int:
    project = load_project(args.project.resolve())
    warnings = validate_project(project)
    print(f"OK: {project.meta['name']} ({len(project.items)} item(s))")
    for warning in warnings:
        print(f"WAARSCHUWING: {warning}")
    return 0


def command_export(args: argparse.Namespace) -> int:
    project = load_project(args.project.resolve())
    warnings = validate_project(project)
    output = args.output.resolve()
    if output.exists() and args.overwrite:
        shutil.rmtree(output)
    output.mkdir(parents=True, exist_ok=True)
    (output / "qr").mkdir(exist_ok=True)

    media_rows = ["# qr-content;relative-media-path;title shown on the display"]
    for item in project.items:
        relative = export_item(project, output, item)
        update_media_map(media_rows, item, relative)
        make_qr_label(project, item, output / "qr" / f"{item.identifier}.png")
    (output / "media-map.csv").write_text("\n".join(media_rows) + "\n", encoding="utf-8")
    (output / "project-export.json").write_text(json.dumps(project.meta, indent=2, ensure_ascii=False) + "\n",
                                                 encoding="utf-8")
    count = create_qr_sheet(output / "qr", output / "qr-labels-a4.pdf")
    print(f"Export klaar: {output}")
    print(f"QR-labels: {count}")
    for warning in warnings:
        print(f"WAARSCHUWING: {warning}")
    return 0


def command_qr_sheet(args: argparse.Namespace) -> int:
    count = create_qr_sheet(args.qr_dir.resolve(), args.output.resolve())
    print(f"QR-PDF klaar: {args.output} ({count} labels)")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    validate = sub.add_parser("validate", help="controleer een Lamp Studio project")
    validate.add_argument("project", type=Path)
    validate.set_defaults(func=command_validate)

    export = sub.add_parser("export", help="maak een SD-export")
    export.add_argument("project", type=Path)
    export.add_argument("--output", type=Path, default=Path("sd-export"))
    export.add_argument("--overwrite", action="store_true")
    export.set_defaults(func=command_export)

    sheet = sub.add_parser("qr-sheet", help="maak een A4 PDF uit QR-label PNG's")
    sheet.add_argument("qr_dir", type=Path)
    sheet.add_argument("--output", type=Path, default=Path("sd-export/qr-labels-a4.pdf"))
    sheet.set_defaults(func=command_qr_sheet)
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    try:
        return args.func(args)
    except (ValueError, subprocess.CalledProcessError) as error:
        parser.error(str(error))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
