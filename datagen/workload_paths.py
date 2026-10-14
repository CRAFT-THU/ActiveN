"""Resolve immutable retained inputs without recreating the old experiment tree."""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[1]


def load_catalog(path: Path | None = None, *, repository: Path = REPOSITORY) -> dict:
    path = path or repository / 'workloads/retained.json'
    return json.loads(path.read_text()) if path.is_file() else {'assets': {}}


def resolve_retained_path(value: str | Path, *, archive_root: Path | None = None,
                          repository: Path = REPOSITORY, catalog: dict | None = None) -> Path:
    """Resolve an asset ID, current path, or recorded pre-migration input path.

    An explicit archive_root takes precedence for legacy paths/asset IDs. Without
    it, prefer the extracted immutable input, then the repository's archive.
    Ordinary explicit paths outside the old tree retain their normal meaning.
    """
    repository = Path(repository).resolve()
    catalog = load_catalog(repository=repository) if catalog is None else catalog
    assets = catalog.get('assets', {})
    text = str(value)
    asset = assets.get(text)
    if asset is not None:
        current = repository / asset['path']
        if archive_root is None and current.exists():
            return current
        text = asset['source']
    original = Path(text)
    normalized = Path(os.path.normpath(text))
    relative = None
    if not normalized.is_absolute() and normalized.parts[:2] == ('copilot', 'tmp'):
        relative = normalized
    for root in [repository, *map(Path, catalog.get('legacy_roots', []))]:
        try:
            candidate = normalized.relative_to(root)
        except ValueError:
            continue
        if candidate.parts[:2] == ('copilot', 'tmp'):
            relative = candidate
            break
    if relative is not None:
        if archive_root is None:
            for item in sorted(assets.values(), key=lambda a: len(Path(a['source']).parts), reverse=True):
                try:
                    tail = relative.relative_to(item['source'])
                except ValueError:
                    continue
                extracted = repository / item['path'] / tail
                if extracted.exists():
                    return extracted
        archived = Path(archive_root) if archive_root is not None else repository / 'archive/tmp'
        archived = archived / relative.relative_to('copilot/tmp')
        if archived.exists():
            return archived
        # This fallback permits inspection during the migration only; it never
        # creates the old tree and is disabled by an explicit archive root.
        old = original if original.is_absolute() else repository / original
        if archive_root is None and old.exists():
            return old
        raise FileNotFoundError(f'retained input is unavailable: {value}; expected {archived}')
    if original.exists():
        return original.resolve()
    if not original.is_absolute() and (repository / original).exists():
        return (repository / original).resolve()
    raise FileNotFoundError(f'input is unavailable: {value}')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', nargs='?', help='catalog asset ID or input path')
    parser.add_argument('--archive-root', type=Path, help='mounted archive/tmp root')
    parser.add_argument('--list', action='store_true', help='list catalog asset IDs and active locations')
    args = parser.parse_args()
    catalog = load_catalog()
    if args.list:
        for name, item in sorted(catalog['assets'].items()):
            print(f"{name}\t{item['path']}")
    elif args.input:
        print(resolve_retained_path(args.input, archive_root=args.archive_root, catalog=catalog))
    else:
        parser.error('supply an input or --list')


if __name__ == '__main__':
    main()
