#!/usr/bin/env python3
"""Generate Dutch MP3 narrations for the QR museum lamp with OpenAI TTS.

The script uses only Python's standard library.  It reads the API key from a
local, git-ignored ``OPENAI_API_KEY.env`` file (or from ``OPENAI_API_KEY`` in
the environment) and writes one MP3 beside each ``gss-*.txt`` narration.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_NARRATION_DIR = PROJECT_ROOT / "sdcard-example" / "narration"
DEFAULT_ENV_FILE = PROJECT_ROOT / "OPENAI_API_KEY.env"
DEFAULT_SAMPLE_DIR = PROJECT_ROOT / "output" / "voice-samples"
API_URL = "https://api.openai.com/v1/audio/speech"
MAX_INPUT_CHARS = 4096
KNOWN_VOICES = {
    "alloy", "ash", "ballad", "coral", "echo", "fable", "onyx", "nova",
    "sage", "shimmer", "verse", "marin", "cedar",
}
DEFAULT_INSTRUCTIONS = (
    "Spreek uitsluitend Nederlands. Gebruik een warme, vlotte en zichtbaar "
    "enthousiaste stem, zoals een bevlogen museumgids die een groep direct uitnodigt "
    "om mee op ontdekking te gaan. Houd het tempo energiek en maak de zinnen "
    "vooruitstrevend, met duidelijke variatie in intonatie. Leg extra nieuwsgierigheid "
    "in historische momenten en uitnodigingen. Blijf geloofwaardig en vriendelijk, "
    "nooit schreeuwerig of reclame-achtig. Articuleer namen, jaartallen, afmetingen "
    "en technische termen zorgvuldig. Voeg geen tekst toe en verander de aangeleverde "
    "tekst niet."
)


def read_api_key(env_file: Path) -> str:
    """Read a key without ever printing it.

    Both a normal ``OPENAI_API_KEY=sk-...`` file and a file containing only
    the key are accepted.  This keeps setup simple while avoiding a shell
    ``source`` operation on a secrets file.
    """
    key = os.environ.get("OPENAI_API_KEY", "").strip()
    if key:
        return key
    if not env_file.is_file():
        raise RuntimeError(
            f"API-sleutel ontbreekt: maak {env_file.name} in de projectroot "
            "met OPENAI_API_KEY=..."
        )
    for raw_line in env_file.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[7:].strip()
        if line.startswith("OPENAI_API_KEY="):
            key = line.split("=", 1)[1].strip().strip("\"'")
        elif line.startswith("sk-"):
            key = line.strip("\"'")
        if key:
            return key
    raise RuntimeError(f"Geen OPENAI_API_KEY gevonden in {env_file.name}")


def create_speech(api_key: str, text: str, destination: Path, args: argparse.Namespace) -> None:
    payload = {
        "model": args.model,
        "voice": args.voice,
        "input": text,
        "instructions": args.instructions,
        "response_format": "mp3",
        "speed": args.speed,
    }
    request = urllib.request.Request(
        API_URL,
        data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
            "User-Agent": "QR-museumlamp-narration/1.0",
        },
        method="POST",
    )
    temporary = destination.with_suffix(".mp3.part")
    try:
        with urllib.request.urlopen(request, timeout=args.timeout) as response:
            data = response.read()
    except urllib.error.HTTPError as error:
        details = error.read().decode("utf-8", "replace")[:600]
        raise RuntimeError(f"OpenAI gaf HTTP {error.code}: {details}") from error
    except urllib.error.URLError as error:
        raise RuntimeError(f"OpenAI is niet bereikbaar: {error.reason}") from error

    # An MP3 can start with an ID3 tag or with an MPEG frame sync.  At 24 kHz
    # the encoder may use MPEG-2/2.5 (for example FF F3), not only FF FB.
    has_mpeg_frame_sync = len(data) >= 2 and data[0] == 0xFF and (data[1] & 0xE0) == 0xE0
    if len(data) < 128 or (not data.startswith(b"ID3") and not has_mpeg_frame_sync):
        raise RuntimeError("De API gaf geen herkenbaar MP3-bestand terug")
    temporary.write_bytes(data)
    temporary.replace(destination)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Maak Nederlandse MP3-vertellingen met OpenAI TTS.")
    parser.add_argument("--input", type=Path, default=DEFAULT_NARRATION_DIR,
                        help="map met gss-*.txt (standaard: sdcard-example/narration)")
    parser.add_argument("--env-file", type=Path, default=DEFAULT_ENV_FILE,
                        help="lokaal bestand met OPENAI_API_KEY (wordt nooit getoond)")
    parser.add_argument("--model", default="gpt-4o-mini-tts")
    parser.add_argument("--file", type=Path,
                        help="één tekstbestand voor een stemproef, bijvoorbeeld narration/welkom.txt")
    parser.add_argument("--output-dir", type=Path,
                        help="map voor stemproeven; bestandsnamen krijgen automatisch -<stem>.mp3")
    parser.add_argument("--voice", action="append", default=[],
                        help="TTS-stem; herhaal de optie voor meerdere proeven (standaard: marin)")
    parser.add_argument("--speed", type=float, default=1.05,
                        help="spreeksnelheid, 0.25 t/m 4.0 (standaard: 1.05)")
    parser.add_argument("--instructions", default=DEFAULT_INSTRUCTIONS)
    parser.add_argument("--overwrite", action="store_true", help="genereer ook bestaande MP3's opnieuw")
    parser.add_argument("--limit", type=int, help="genereer hoogstens dit aantal bestanden, voor een stemtest")
    parser.add_argument("--dry-run", action="store_true", help="toon wat er gemaakt zou worden, zonder API-aanroep")
    parser.add_argument("--timeout", type=float, default=90, help="maximale wachttijd per API-aanroep in seconden")
    args = parser.parse_args()
    if not 0.25 <= args.speed <= 4.0:
        parser.error("--speed moet tussen 0.25 en 4.0 liggen")
    if args.limit is not None and args.limit < 1:
        parser.error("--limit moet minimaal 1 zijn")
    args.voice = args.voice or ["marin"]
    unknown_voices = sorted(set(args.voice) - KNOWN_VOICES)
    if unknown_voices:
        parser.error("onbekende stem: " + ", ".join(unknown_voices))
    if args.file is not None and args.limit is not None:
        parser.error("gebruik --file niet samen met --limit")
    return args


def main() -> int:
    args = parse_arguments()
    if args.file is not None:
        sources = [args.file.resolve()]
        if not sources[0].is_file():
            print(f"FOUT: tekstbestand bestaat niet: {sources[0]}", file=sys.stderr)
            return 2
    else:
        source_dir = args.input.resolve()
        if not source_dir.is_dir():
            print(f"FOUT: map bestaat niet: {source_dir}", file=sys.stderr)
            return 2
        sources = sorted(source_dir.glob("gss-*.txt"))
        if not sources:
            print(f"FOUT: geen gss-*.txt gevonden in {source_dir}", file=sys.stderr)
            return 2
        if args.limit is not None:
            sources = sources[:args.limit]

    output_dir = args.output_dir.resolve() if args.output_dir else None
    jobs: list[tuple[Path, str, Path]] = []
    for source in sources:
        for voice in args.voice:
            is_voice_comparison = len(args.voice) > 1 or output_dir is not None
            destination = ((output_dir or source.parent) / f"{source.stem}-{voice}.mp3"
                           if is_voice_comparison else source.with_suffix(".mp3"))
            if args.overwrite or not destination.exists():
                jobs.append((source, voice, destination))

    print(f"{len(sources)} verteltekst(en), {len(jobs)} MP3-bestand(en) te maken; "
          f"stem(men): {', '.join(args.voice)}.")
    if args.dry_run:
        for source, voice, destination in jobs:
            print(f"  {source.name} [{voice}] -> {destination}")
        return 0
    if not jobs:
        return 0

    try:
        api_key = read_api_key(args.env_file.resolve())
    except RuntimeError as error:
        print(f"FOUT: {error}", file=sys.stderr)
        return 2

    failures = 0
    for index, (source, voice, destination) in enumerate(jobs, start=1):
        text = source.read_text(encoding="utf-8").strip()
        if not text:
            print(f"FOUT: {source.name} is leeg", file=sys.stderr)
            failures += 1
            continue
        if len(text) > MAX_INPUT_CHARS:
            print(f"FOUT: {source.name} is langer dan {MAX_INPUT_CHARS} tekens", file=sys.stderr)
            failures += 1
            continue
        destination.parent.mkdir(parents=True, exist_ok=True)
        print(f"[{index}/{len(jobs)}] {source.name} [{voice}] -> {destination.name}", flush=True)
        try:
            call_args = argparse.Namespace(**vars(args))
            call_args.voice = voice
            create_speech(api_key, text, destination, call_args)
        except RuntimeError as error:
            print(f"FOUT: {source.name}: {error}", file=sys.stderr)
            failures += 1
            continue
        # Keep batch calls polite and give a transient network error less chance
        # to affect the next item.
        time.sleep(0.15)

    if failures:
        print(f"Klaar met {failures} fout(en); voer het script opnieuw uit om alleen ontbrekende MP3's te proberen.",
              file=sys.stderr)
        return 1
    print("Klaar. Kopieer de nieuwe narration/*.mp3-bestanden naar de SD-kaart.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
