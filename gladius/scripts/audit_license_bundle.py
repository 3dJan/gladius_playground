#!/usr/bin/env python3
"""Check curated notices against a profile's installed vcpkg package notices."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[1]
INVENTORY_PATH = Path(__file__).with_name("license_inventory.json")


def load_inventory(path: Path = INVENTORY_PATH) -> dict[str, Any]:
    with path.open(encoding="utf-8") as inventory_file:
        return json.load(inventory_file)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as asset_file:
        for block in iter(lambda: asset_file.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def audit_profile(
    repo_root: Path,
    triplet_root: Path,
    profile: dict[str, Any],
    inventory: dict[str, Any],
) -> list[str]:
    errors: list[str] = []
    packages = inventory["packages"]
    seen: set[str] = set()
    required_shared_licenses: set[str] = set()

    for package_name in profile["dependencies"]:
        if package_name in seen:
            errors.append(f"duplicate dependency entry: {package_name}")
            continue
        seen.add(package_name)

        package = packages.get(package_name)
        if package is None:
            errors.append(f"{package_name}: missing package mapping in inventory")
            continue

        installed_notice = triplet_root / "share" / package["share"] / "copyright"
        if not installed_notice.is_file():
            spdx_notice = installed_notice.with_name("vcpkg.spdx.json")
            if not package.get("copyright_optional") or not spdx_notice.is_file():
                errors.append(
                    f"{package_name}: installed package notice missing: {installed_notice}"
                )

        notice_dir = repo_root / "components" / "licenses" / package["notice"]
        required_shared_licenses.update(package.get("shared_licenses", []))
        for filename in ["LICENSE", "url", *package.get("additional_files", [])]:
            notice_file = notice_dir / filename
            if not notice_file.is_file() or notice_file.stat().st_size == 0:
                errors.append(f"{package_name}: curated notice file missing or empty: {notice_file}")

    for asset in inventory.get("assets", []):
        required_shared_licenses.update(asset.get("shared_licenses", []))
        asset_path = repo_root / asset["path"]
        if not asset_path.is_file():
            errors.append(f"font asset missing: {asset_path}")
        elif sha256_file(asset_path) != asset["sha256"]:
            errors.append(f"font asset hash changed: {asset_path}")

        notice_dir = repo_root / "components" / "licenses" / asset["notice"]
        for filename in ("LICENSE", "url"):
            notice_file = notice_dir / filename
            if not notice_file.is_file() or notice_file.stat().st_size == 0:
                errors.append(f"font notice file missing or empty: {notice_file}")

    shared_licenses = inventory.get("shared_licenses", {})
    for license_name in sorted(required_shared_licenses):
        shared_license = shared_licenses.get(license_name)
        if shared_license is None:
            errors.append(f"{license_name}: missing shared-license mapping in inventory")
            continue

        notice_dir = repo_root / "components" / "licenses" / shared_license["notice"]
        for filename in ("LICENSE", "url"):
            notice_file = notice_dir / filename
            if not notice_file.is_file() or notice_file.stat().st_size == 0:
                errors.append(f"{license_name}: shared license file missing or empty: {notice_file}")

    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("web", "linux"), required=True)
    parser.add_argument(
        "--triplet-root",
        type=Path,
        help="vcpkg triplet directory containing share/ (overrides inventory default)",
    )
    parser.add_argument(
        "--inventory",
        type=Path,
        default=INVENTORY_PATH,
        help="path to the license inventory JSON",
    )
    args = parser.parse_args(argv)

    inventory = load_inventory(args.inventory)
    profile = inventory["profiles"][args.profile]
    triplet_root = args.triplet_root or (REPO_ROOT / profile["triplet_root"])
    if not triplet_root.is_absolute():
        triplet_root = REPO_ROOT / triplet_root

    errors = audit_profile(REPO_ROOT, triplet_root, profile, inventory)
    for exclusion in profile.get("excluded", []):
        print(f"Excluded {exclusion['package']}: {exclusion['reason']}")

    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1

    print(
        f"License bundle audit passed for {args.profile}: "
        f"{len(profile['dependencies'])} package notices and "
        f"shared license texts, and {len(inventory.get('assets', []))} font assets checked."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
