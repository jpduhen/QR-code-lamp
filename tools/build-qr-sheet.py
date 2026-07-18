#!/usr/bin/env python3
"""Create print-ready A4 sheets from the museum QR-label PNG files."""

from __future__ import annotations

import argparse
import re
from pathlib import Path

from reportlab.lib.pagesizes import A4
from reportlab.lib.utils import ImageReader
from reportlab.pdfgen import canvas


COLUMNS = 3
ROWS = 7
MARGIN_MM = 7
GUTTER_MM = 1.5
MM = 72 / 25.4


def natural_key(path: Path) -> list[object]:
    return [int(part) if part.isdigit() else part.lower() for part in re.split(r"(\d+)", path.stem)]


def create_pdf(source: Path, output: Path) -> int:
    labels = sorted(
        (path for path in source.glob("*.png") if not path.name.startswith("._")),
        key=natural_key,
    )
    if not labels:
        raise ValueError(f"Geen PNG QR-codes gevonden in {source}")

    output.parent.mkdir(parents=True, exist_ok=True)
    page_width, page_height = A4
    margin = MARGIN_MM * MM
    gutter = GUTTER_MM * MM
    cell_width = (page_width - 2 * margin - (COLUMNS - 1) * gutter) / COLUMNS
    cell_height = (page_height - 2 * margin - (ROWS - 1) * gutter) / ROWS

    pdf = canvas.Canvas(str(output), pagesize=A4, pageCompression=1)
    pdf.setTitle("Gelders Smalspoormuseum QR-codes")
    for number, label in enumerate(labels):
        slot = number % (COLUMNS * ROWS)
        if slot == 0 and number:
            pdf.showPage()
        row, column = divmod(slot, COLUMNS)
        cell_x = margin + column * (cell_width + gutter)
        cell_y = page_height - margin - (row + 1) * cell_height - row * gutter

        image = ImageReader(str(label))
        image_width, image_height = image.getSize()
        scale = min(cell_width / image_width, cell_height / image_height)
        draw_width = image_width * scale
        draw_height = image_height * scale
        draw_x = cell_x + (cell_width - draw_width) / 2
        draw_y = cell_y + (cell_height - draw_height) / 2
        pdf.drawImage(image, draw_x, draw_y, draw_width, draw_height, mask="auto")

        # Fine grey trim marks keep the printable labels easy to cut out.
        pdf.setStrokeColorRGB(0.75, 0.75, 0.75)
        pdf.setLineWidth(0.25)
        pdf.rect(cell_x, cell_y, cell_width, cell_height, stroke=1, fill=0)

    pdf.save()
    return len(labels)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=Path("sdcard-example/qr"))
    parser.add_argument("--output", type=Path, default=Path("output/pdf/museum-qr-codes-a4.pdf"))
    args = parser.parse_args()
    count = create_pdf(args.input, args.output)
    pages = (count + COLUMNS * ROWS - 1) // (COLUMNS * ROWS)
    print(f"Created {args.output} with {count} QR labels on {pages} A4 page(s).")


if __name__ == "__main__":
    main()
