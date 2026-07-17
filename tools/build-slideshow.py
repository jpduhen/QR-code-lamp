#!/usr/bin/env python3
"""Package a timed audio slideshow for the ESP32-S3 museum lamp.

The preferred workflow is a work directory containing exactly one .mp3 or
.wav narration and source images named `hh.mm.ss.mmm` after their start time:

    work/gss-001/gss-001-uitleg.mp3
    work/gss-001/00.00.00.000.png
    work/gss-001/00.00.08.400.jpg
    work/gss-001/00.00.17.300.png

The legacy --audio/--timeline form remains available for existing projects.
"""

from __future__ import annotations

import argparse
import csv
import re
import shutil
from pathlib import Path

import qrcode
from PIL import Image, ImageDraw, ImageFont, ImageOps


CANVAS = (480, 272)
ALLOWED_AUDIO = {".wav", ".mp3"}
TIMESTAMP_NAME = re.compile(r"^(\d{2})\.(\d{2})\.(\d{2})\.(\d{3})$")


def parse_timeline(path: Path) -> list[tuple[int, Path]]:
    rows: list[tuple[int, Path]] = []
    with path.open(newline="", encoding="utf-8") as file:
        for line_number, row in enumerate(csv.reader(file, delimiter=";"), start=1):
            if not row or not row[0].strip() or row[0].lstrip().startswith("#"):
                continue
            if len(row) != 2:
                raise ValueError(f"{path}:{line_number}: verwacht tijd-in-ms;afbeelding")
            try:
                start_ms = int(row[0].strip())
            except ValueError as error:
                raise ValueError(f"{path}:{line_number}: ongeldige tijd") from error
            source = (path.parent / row[1].strip()).resolve()
            if start_ms < 0 or not source.is_file():
                raise ValueError(f"{path}:{line_number}: ontbrekende afbeelding of negatieve tijd")
            if rows and start_ms <= rows[-1][0]:
                raise ValueError(f"{path}:{line_number}: tijden moeten oplopend zijn")
            rows.append((start_ms, source))
    if not rows or rows[0][0] != 0:
        raise ValueError("de eerste dia moet op 0 ms beginnen")
    return rows


def parse_workdir(path: Path) -> tuple[Path, list[tuple[int, Path]]]:
    audio_files = [item for item in path.iterdir() if item.is_file() and item.suffix.lower() in ALLOWED_AUDIO]
    if len(audio_files) != 1:
        raise ValueError(f"{path}: verwacht precies één .wav- of .mp3-bestand")
    rows: list[tuple[int, Path]] = []
    for item in path.iterdir():
        if not item.is_file() or item.suffix.lower() not in {".jpg", ".jpeg", ".png"}:
            continue
        match = TIMESTAMP_NAME.fullmatch(item.stem)
        if match:
            hours, minutes, seconds, milliseconds = (int(value) for value in match.groups())
            if minutes >= 60 or seconds >= 60:
                raise ValueError(f"{item.name}: minuten en seconden moeten lager dan 60 zijn")
            start_ms = (((hours * 60) + minutes) * 60 + seconds) * 1000 + milliseconds
        else:
            # Compatibility with 8400.jpg from the first tool version.
            try:
                start_ms = int(item.stem)
            except ValueError:
                continue
        if start_ms < 0:
            raise ValueError(f"{item.name}: starttijd moet positief zijn")
        rows.append((start_ms, item))
    rows.sort(key=lambda row: row[0])
    if not rows or rows[0][0] != 0:
        raise ValueError(f"{path}: de eerste dia moet 00.00.00.000.jpg/png heten")
    if len({start for start, _ in rows}) != len(rows):
        raise ValueError(f"{path}: twee dia's hebben dezelfde starttijd")
    return audio_files[0], rows


def make_slide(source: Path, destination: Path) -> None:
    image = Image.open(source).convert("RGB")
    # Do not stretch a PowerPoint slide; fill unused space with black.
    image = ImageOps.contain(image, CANVAS, Image.Resampling.LANCZOS)
    canvas = Image.new("RGB", CANVAS, "black")
    canvas.paste(image, ((CANVAS[0] - image.width) // 2, (CANVAS[1] - image.height) // 2))
    canvas.save(destination, "JPEG", quality=84, optimize=True, progressive=False)


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


def wrap(draw: ImageDraw.ImageDraw, text: str, text_font: ImageFont.ImageFont,
         width: int, maximum_lines: int) -> list[str]:
    output: list[str] = []
    current = ""
    for word in text.split():
        candidate = word if not current else f"{current} {word}"
        if draw.textlength(candidate, font=text_font) <= width:
            current = candidate
        else:
            output.append(current)
            current = word
    if current:
        output.append(current)
    if len(output) <= maximum_lines:
        return output
    output = output[:maximum_lines]
    output[-1] = output[-1].rstrip(" .") + "…"
    return output


def make_qr_label(code: str, title: str, destination: Path) -> None:
    qr = qrcode.QRCode(version=None, error_correction=qrcode.constants.ERROR_CORRECT_M,
                       box_size=12, border=4)
    qr.add_data(code)
    qr.make(fit=True)
    code_image = qr.make_image(fill_color="black", back_color="white").convert("RGB")
    code_image.thumbnail((300, 300), Image.Resampling.NEAREST)
    label = Image.new("RGB", (480, 440), "white")
    draw = ImageDraw.Draw(label)
    draw.rectangle((0, 0, 479, 38), fill="#103c6b")
    draw.text((12, 9), "Gelders Smalspoormuseum", fill="white", font=font(18, True))
    y = 50
    for line in wrap(draw, title, font(16, True), 450, 2):
        draw.text((15, y), line, fill="#111111", font=font(16, True))
        y += 19
    label.paste(code_image, ((480 - code_image.width) // 2, 105))
    draw.text((15, 415), f"Scan met de museumlamp  •  {code}", fill="#103c6b", font=font(13))
    label.save(destination, "PNG")


def update_media_map(path: Path, code: str, presentation_path: Path, title: str) -> None:
    root = path.parent.resolve()
    try:
        relative = presentation_path.resolve().relative_to(root).as_posix()
    except ValueError as error:
        raise ValueError("--output moet onder dezelfde SD-hoofdmap staan als --media-map") from error
    header = "# qr-content;relative-media-path;title shown on the display"
    existing = path.read_text(encoding="utf-8").splitlines() if path.exists() else [header]
    retained = [line for line in existing if not line.startswith(f"{code};")]
    path.write_text("\n".join(retained + [f"{code};{relative};{title}"]) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--input", type=Path, help="workmap met één audiofile en dia's als 00.00.08.400.jpg, …")
    source.add_argument("--timeline", type=Path, help="legacy timing CSV")
    parser.add_argument("--audio", type=Path, help="WAV or MP3 narration (alleen samen met --timeline)")
    parser.add_argument("--sd-root", type=Path, default=Path("sdcard-example"),
                        help="hoofdmappen van de SD-inhoud (default: sdcard-example)")
    parser.add_argument("--output", type=Path, help="optionele doelmap; default is shows/<audiobestandsnaam>")
    parser.add_argument("--code", help="optionele QR-inhoud; default is de audiobestandsnaam zonder extensie")
    parser.add_argument("--title", help="optionele titel; default is de audiobestandsnaam leesbaar gemaakt")
    parser.add_argument("--media-map", type=Path, help="werk deze media-mapregel automatisch bij")
    parser.add_argument("--qr-output", type=Path, help="map voor printklare QR-labels")
    args = parser.parse_args()

    if args.input:
        audio, timeline = parse_workdir(args.input.resolve())
    else:
        if args.audio is None:
            raise SystemExit("--audio is verplicht samen met --timeline")
        audio = args.audio.resolve()
        if not audio.is_file() or audio.suffix.lower() not in ALLOWED_AUDIO:
            raise SystemExit("--audio moet een bestaand .wav- of .mp3-bestand zijn")
        timeline = parse_timeline(args.timeline.resolve())
    # The narration filename is the stable identifier throughout the workflow.
    # Keep it scanner-friendly: gss-001-uitleg.mp3 -> gss-001-uitleg.
    identifier = audio.stem
    if not identifier or any(character not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-" for character in identifier):
        raise SystemExit("gebruik voor de audiobestandsnaam alleen letters, cijfers, _ en -, bijvoorbeeld gss-001-uitleg.mp3")
    sd_root = args.sd_root.resolve()
    output = args.output.resolve() if args.output else sd_root / "shows" / identifier
    code = args.code or identifier
    title = args.title or identifier.replace("-", " ").replace("_", " ").upper()
    slides = output / "slides"
    slides.mkdir(parents=True, exist_ok=True)

    audio_name = f"audio{audio.suffix.lower()}"
    shutil.copy2(audio, output / audio_name)
    show_rows = [f"audio;{audio_name}"]
    for index, (start_ms, source) in enumerate(timeline, start=1):
        target_name = f"slides/{index:03d}.jpg"
        make_slide(source, output / target_name)
        show_rows.append(f"slide;{start_ms};{target_name}")
    (output / "show.csv").write_text("# audio + slideshow timeline, generated by build-slideshow.py\n" +
                                       "\n".join(show_rows) + "\n", encoding="utf-8")
    (output / "timings.csv").write_text("start_ms;bronbestand;dia\n" + "\n".join(
        f"{start};{source.name};slides/{index:03d}.jpg"
        for index, (start, source) in enumerate(timeline, start=1)) + "\n", encoding="utf-8")
    media_map = args.media_map.resolve() if args.media_map else sd_root / "media-map.csv"
    update_media_map(media_map, code, output / "show.csv", title)
    qr_output = args.qr_output.resolve() if args.qr_output else sd_root / "qr"
    qr_output.mkdir(parents=True, exist_ok=True)
    make_qr_label(code, title, qr_output / f"{code}.png")
    print(f"Klaar: {len(timeline)} dia's en {audio_name} in {output} (QR: {code})")


if __name__ == "__main__":
    main()
