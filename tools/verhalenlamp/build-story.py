#!/usr/bin/env python3
"""Build one offline Verhalenlamp halt from a content work directory.

The existing tools/build-slideshow.py performs all slideshow packaging. This
tool only validates story.md/meta.json and fixes the Verhalenlamp export paths.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
SLIDESHOW_BUILDER = REPOSITORY_ROOT / "tools" / "build-slideshow.py"
IDENTIFIER = re.compile(r"^[a-z0-9][a-z0-9_-]*$")
HEADING = re.compile(r"^#\s+(.+?)\s*$", re.MULTILINE)


def read_story(source: Path) -> str:
    path = source / "story.md"
    if not path.is_file():
        raise ValueError(f"ontbrekend bronbestand: {path}")
    story = path.read_text(encoding="utf-8").strip()
    if not story:
        raise ValueError(f"{path} is leeg")
    return story


def read_meta(source: Path) -> dict[str, Any]:
    path = source / "meta.json"
    if not path.is_file():
        raise ValueError(f"ontbrekend bronbestand: {path}")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise ValueError(f"ongeldige JSON in {path}: {error.msg}") from error
    if not isinstance(value, dict):
        raise ValueError(f"{path} moet een JSON-object bevatten")
    return value


def heading(story: str) -> str:
    match = HEADING.search(story)
    return match.group(1).strip() if match else ""


def validate_identifier(source: Path, meta: dict[str, Any]) -> str:
    value = meta.get("id", source.name)
    if not isinstance(value, str) or not IDENTIFIER.fullmatch(value):
        raise ValueError("meta.json veld 'id' moet kleine letters, cijfers, _ en - bevatten")
    if source.name != value:
        raise ValueError(f"mapnaam '{source.name}' moet gelijk zijn aan meta.json id '{value}'")
    return value


def validate_title(meta: dict[str, Any], story: str, identifier: str) -> str:
    value = meta.get("title", heading(story) or identifier.replace("-", " ").upper())
    if not isinstance(value, str) or not value.strip():
        raise ValueError("meta.json veld 'title' moet een niet-lege tekst zijn")
    if any(character in value for character in ";\r\n"):
        raise ValueError("meta.json veld 'title' mag geen puntkomma of regeleinde bevatten")
    return value.strip()


def require_audio(source: Path) -> None:
    audio = source / "audio.mp3"
    if not audio.is_file() or audio.stat().st_size == 0:
        raise ValueError(f"verwacht een niet-leeg MP3-bestand: {audio}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True,
                        help="bronmap, bijvoorbeeld work/ringoven")
    parser.add_argument("--output", type=Path, default=Path("sd-export"),
                        help="SD-exportmap (standaard: sd-export)")
    parser.add_argument("--overwrite", action="store_true",
                        help="vervang een bestaande showmap voor deze halte")
    parser.add_argument("--dry-run", action="store_true",
                        help="valideer bron en toon paden zonder export te maken")
    args = parser.parse_args()

    source = args.input.resolve()
    if not source.is_dir():
        parser.error(f"bronmap bestaat niet: {source}")
    if not SLIDESHOW_BUILDER.is_file():
        parser.error(f"bestaande slideshowtool ontbreekt: {SLIDESHOW_BUILDER}")

    try:
        story = read_story(source)
        meta = read_meta(source)
        identifier = validate_identifier(source, meta)
        title = validate_title(meta, story, identifier)
        require_audio(source)
    except ValueError as error:
        parser.error(str(error))

    export_root = args.output.resolve()
    show = export_root / "shows" / identifier
    media_map = export_root / "media-map.csv"
    qr_output = export_root / "qr"
    if show.exists():
        if not args.overwrite:
            parser.error(f"export bestaat al: {show} (gebruik --overwrite om deze halte te vervangen)")
        if not args.dry_run:
            shutil.rmtree(show)

    command = [
        sys.executable, str(SLIDESHOW_BUILDER),
        "--input", str(source),
        "--sd-root", str(export_root),
        "--output", str(show),
        "--code", identifier,
        "--title", title,
        "--media-map", str(media_map),
        "--qr-output", str(qr_output),
    ]
    if args.dry_run:
        print(f"Geldig: {identifier} — {title}")
        print(f"Show: {show}")
        print(f"Index: {media_map}")
        print(f"QR: {qr_output / (identifier + '.png')}")
        return 0

    subprocess.run(command, check=True)
    print(f"Verhalenlamp-halte klaar: {identifier}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
