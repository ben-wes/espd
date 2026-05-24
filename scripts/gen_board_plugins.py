#!/usr/bin/env python3
"""
Generate espd_board_* plugin directories from boards/*.yaml.

Run automatically from root CMakeLists.txt before Kconfig / component discovery.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

try:
    import yaml
except ImportError as exc:  # pragma: no cover
    raise SystemExit(
        "PyYAML is required (install in IDF Python env: pip install pyyaml)"
    ) from exc

ID_RE = re.compile(r"^[a-z][a-z0-9_]*$")
GENERATED_HEADER = "# Auto-generated from {src} — do not edit.\n"
CMAKE_GLUE = """\
# Auto-generated from {src} — do not edit.

if(CONFIG_ESPD_BOARD_ESP_BSP_GLUE)
    set(_espd_glue "${{CMAKE_CURRENT_LIST_DIR}}/../espd_integration")
    idf_component_register(
        SRCS
            "${{_espd_glue}}/espd_bsp_esp_bsp_audio.c"
            "${{_espd_glue}}/espd_bsp_esp_bsp_io.c"
        INCLUDE_DIRS "."
    )
else()
    idf_component_register()
endif()
"""


def _config_key(key: str) -> str:
    key = str(key).strip()
    if key.startswith("CONFIG_"):
        return key[len("CONFIG_") :]
    return key


def _config_line(key: str, value) -> str:
    k = _config_key(key)
    if value is None or value == "":
        return f"CONFIG_{k}=\n"
    if isinstance(value, bool):
        return f"CONFIG_{k}={'y' if value else 'n'}\n"
    return f"CONFIG_{k}={value}\n"


def _load_board(path: Path) -> dict:
    data = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError(f"{path}: expected mapping at top level")
    return data


def _validate_board(data: dict, path: Path) -> None:
    board_id = data.get("id")
    if not board_id or not ID_RE.match(str(board_id)):
        raise ValueError(f"{path}: id must match {ID_RE.pattern!r}")
    if not data.get("name"):
        raise ValueError(f"{path}: missing name")
    if not data.get("target"):
        raise ValueError(f"{path}: missing target")
    bsp = data.get("bsp")
    if not isinstance(bsp, dict) or not bsp.get("component"):
        raise ValueError(f"{path}: bsp.component is required")


def _kconfig_symbol(board_id: str) -> str:
    return f"ESPD_BOARD_{board_id.upper()}"


def _gen_kconfig(data: dict, src: str) -> str:
    sym = _kconfig_symbol(data["id"])
    target = data["target"]
    name = data["name"]
    help_text = (data.get("help") or name).strip()
    lines = [
        GENERATED_HEADER.format(src=src),
        f"config {sym}\n",
        f'    bool "{name}"\n',
        f"    depends on IDF_TARGET_{target.upper()}\n",
        "    select ESPD_BOARD_ESP_BSP_GLUE\n",
    ]
    for feat in data.get("features", {}).get("imply", []) or []:
        lines.append(f"    imply {feat}\n")
    lines.append("    help\n")
    for hl in help_text.splitlines():
        lines.append(f"        {hl}\n")
    return "".join(lines)


def _gen_idf_component_yml(data: dict, src: str) -> str:
    bsp = data["bsp"]
    comp = bsp["component"]
    lines = [
        GENERATED_HEADER.format(src=src),
        'version: "0.1.0"\n',
        f'description: {data["name"]} board plugin for espd\n',
        "dependencies:\n",
        '  idf: ">=6.0.1,<6.1"\n',
        "  espd_integration:\n",
        "    path: ../espd_integration\n",
        f"  {comp}:\n",
    ]
    if "git" in bsp:
        lines.append(f'    git: {bsp["git"]}\n')
        if "path" in bsp:
            lines.append(f'    path: {bsp["path"]}\n')
        if "version" in bsp:
            lines.append(f'    version: {bsp["version"]}\n')
    elif "registry" in bsp or "version" in bsp:
        ver = bsp.get("registry") or bsp.get("version")
        lines.append(f"    version: \"{ver}\"\n")
    else:
        raise ValueError(f"{src}: bsp needs git or registry version")
    return "".join(lines)


def _gen_sdkconfig_defaults(data: dict, src: str) -> str:
    sym = _kconfig_symbol(data["id"])
    lines = [
        GENERATED_HEADER.format(src=src),
        f"# Merged when CONFIG_{sym}=y (see root CMakeLists.txt).\n",
        "\n",
    ]
    profile = data.get("profile") or {}
    if not isinstance(profile, dict):
        raise ValueError(f"{src}: profile must be a mapping")
    for section, options in profile.items():
        lines.append(f"# --- {section} ---\n\n")
        if not isinstance(options, dict):
            raise ValueError(f"{src}: profile.{section} must be a mapping")
        for key, value in options.items():
            lines.append(_config_line(key, value))
        lines.append("\n")
    return "".join(lines)


def _gen_io_config(data: dict, src: str) -> str | None:
    io = data.get("io") or {}
    buttons = io.get("buttons")
    if not buttons:
        return None
    if not isinstance(buttons, list) or not buttons:
        raise ValueError(f"{src}: io.buttons must be a non-empty list")
    board_id = data["id"]
    entries = ", \\\n    ".join(f"BSP_BUTTON_{b}" for b in buttons)
    return (
        "/*\n"
        f" * Auto-generated from {src} — do not edit.\n"
        " *\n"
        f" * Button map for espd_board_{board_id}.\n"
        " */\n"
        "#pragma once\n\n"
        '#include "bsp/esp-bsp.h"\n\n'
        f"#define ESPD_BSP_BUTTON_COUNT {len(buttons)}\n"
        f"#define ESPD_BSP_BUTTON_MAP {{ \\\n    {entries}, \\\n}}\n"
    )


def _write_if_changed(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_text(encoding="utf-8") == content:
        return
    path.write_text(content, encoding="utf-8")


def _generate_board(repo: Path, yaml_path: Path) -> Path:
    rel_src = yaml_path.relative_to(repo).as_posix()
    data = _load_board(yaml_path)
    _validate_board(data, yaml_path)

    out_dir = repo / "components" / f"espd_board_{data['id']}"
    _write_if_changed(out_dir / "Kconfig.board", _gen_kconfig(data, rel_src))
    _write_if_changed(out_dir / "idf_component.yml", _gen_idf_component_yml(data, rel_src))
    _write_if_changed(out_dir / "CMakeLists.txt", CMAKE_GLUE.format(src=rel_src))
    _write_if_changed(out_dir / "sdkconfig.defaults", _gen_sdkconfig_defaults(data, rel_src))

    io_cfg = _gen_io_config(data, rel_src)
    io_path = out_dir / "espd_board_io_config.h"
    if io_cfg:
        _write_if_changed(io_path, io_cfg)
    elif io_path.exists():
        io_path.unlink()

    return out_dir


def main(argv: list[str]) -> int:
    repo = Path(argv[1] if len(argv) > 1 else ".").resolve()
    boards_dir = repo / "boards"
    if not boards_dir.is_dir():
        return 0

    yaml_files = sorted(boards_dir.glob("*.yaml"))
    if not yaml_files:
        return 0

    for yaml_path in yaml_files:
        out = _generate_board(repo, yaml_path)
        print(f"gen_board_plugins: {yaml_path.name} -> {out.relative_to(repo)}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
