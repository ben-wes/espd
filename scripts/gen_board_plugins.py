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
# Standardized peripheral schemas
_SUPPORTED_PERIPHERALS = {
    "sensor": {
        "required": ["bsp_getter", "rate_ms", "outputs"],
    },
    "actuator": {
        "required": ["bsp_setter", "inputs"],
    }
}

def _gen_cmake_glue(data: dict, src: str) -> str:
    bsp_component = data["bsp"]["component"]
    peripherals = data.get("peripherals") or []
    extra_srcs = ""
    if peripherals:
        extra_srcs = '\n            "espd_board_peripherals.c"'
    return f"""# Auto-generated from {src} — do not edit.

if(CONFIG_ESPD_BOARD_ESP_BSP_GLUE)
    idf_component_register(
        SRCS
            "espd_bsp_audio_glue.c"
            "espd_bsp_io_glue.c"{extra_srcs}
        INCLUDE_DIRS "." "${{CMAKE_CURRENT_LIST_DIR}}"
        REQUIRES espd_integration {bsp_component}
    )
else()
    idf_component_register()
endif()
"""

AUDIO_GLUE_C = """\
/*
 * Auto-generated from {src} — do not edit.
 *
 * Board-owned glue TU to keep component source ownership clean while
 * reusing shared espd_integration implementation.
 */
#include "../espd_integration/espd_bsp_esp_bsp_audio.c"
"""

IO_GLUE_C = """\
/*
 * Auto-generated from {src} — do not edit.
 *
 * Board-owned glue TU to keep component source ownership clean while
 * reusing shared espd_integration implementation.
 */
#include "../espd_integration/espd_bsp_esp_bsp_io.c"
"""


def _config_key(key: str) -> str:
    key = str(key).strip()
    if key.startswith("CONFIG_"):
        return key[len("CONFIG_") :]
    return key


def _kconfig_value(value):
    if isinstance(value, bool):
        return value
    if isinstance(value, str):
        s = value.strip()
        if s in ("y", "Y", "yes", "true"):
            return True
        if s in ("n", "N", "no", "false"):
            return False
        return s
    return value


def _config_line(key: str, value) -> str:
    k = _config_key(key)
    value = _kconfig_value(value)
    if value is None or value == "":
        return f"CONFIG_{k}=\n"
    if isinstance(value, bool):
        return f"CONFIG_{k}={'y' if value else 'n'}\n"
    if isinstance(value, str):
        s = value.strip()
        if (s.startswith('"') and s.endswith('"')) or (s.startswith("'") and s.endswith("'")):
            return f"CONFIG_{k}={s}\n"
        return f'CONFIG_{k}="{s}"\n'
    return f"CONFIG_{k}={value}\n"


def _feature_implied(data: dict, symbol: str) -> bool:
    profile = data.get("profile") or {}
    for options in profile.values():
        if isinstance(options, dict):
            val = options.get(symbol)
            if val is not None:
                return bool(_kconfig_value(val))
    return symbol in (data.get("features") or {}).get("imply") or []


def _profile_has_usb_otg(data: dict) -> bool:
    return _feature_implied(data, "ESPD_USE_USB_OTG")


def _profile_has_wifi(data: dict) -> bool:
    return _feature_implied(data, "ESPD_USE_WIFI")


_CONSOLE_PRIMARY_MEMBERS = {
    "ESP_CONSOLE_UART_DEFAULT",
    "ESP_CONSOLE_USB_CDC",
    "ESP_CONSOLE_USB_SERIAL_JTAG",
    "ESP_CONSOLE_UART_CUSTOM",
    "ESP_CONSOLE_NONE",
}
_CONSOLE_SECONDARY_MEMBERS = {
    "ESP_CONSOLE_SECONDARY_NONE",
    "ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG",
}


def _otg_sdkconfig_extras(data: dict, profile_keys: set[str]) -> list[tuple[str, object]]:
    """Hand the shared USB PHY to OTG: console off, USB Serial/JTAG off.

    These are console *choice* members and a driver toggle that Kconfig
    ``select`` cannot set, so boards no longer list them — we derive them from
    ESPD_USE_USB_OTG. A board can still override the primary/secondary console
    by putting any ESP_CONSOLE_* member in its profile (e.g. keep a UART
    primary on a board that exposes one), and the matching default is skipped.
    """
    if not _profile_has_usb_otg(data) or str(data.get("target")) not in (
        "esp32s3",
        "esp32c3",
        "esp32c6",
        "esp32h2",
        "esp32c5",
        "esp32p4",
    ):
        return []
    extras: list[tuple[str, object]] = []
    if not (profile_keys & _CONSOLE_PRIMARY_MEMBERS):
        extras.append(("ESP_CONSOLE_NONE", True))
    if not (profile_keys & _CONSOLE_SECONDARY_MEMBERS):
        extras.append(("ESP_CONSOLE_SECONDARY_NONE", True))
    usj_in_profile = "USJ_ENABLE_USB_SERIAL_JTAG" in profile_keys
    if not usj_in_profile and (
        "ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG" not in profile_keys
        and "ESP_CONSOLE_USB_SERIAL_JTAG" not in profile_keys
    ):
        extras.append(("USJ_ENABLE_USB_SERIAL_JTAG", False))
    return extras


def _wifi_usb_coexist_extras(data: dict, profile_keys: set[str]) -> list[tuple[str, object]]:
    """S3/C3: Wi-Fi PHY init disables USB unless ESP_PHY_ENABLE_USB (IDF default n
    when console is not USB Serial/JTAG). Required for OTG CDC after esp_wifi_init()."""
    if not _profile_has_usb_otg(data) or not _profile_has_wifi(data):
        return []
    if str(data.get("target")) not in (
        "esp32s3",
        "esp32c3",
        "esp32c6",
        "esp32h2",
        "esp32c5",
    ):
        return []
    if "ESP_PHY_ENABLE_USB" in profile_keys:
        return []
    return [("ESP_PHY_ENABLE_USB", True)]


def _load_board(path: Path) -> dict:
    data = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError(f"{path}: expected mapping at top level")
    return data


def _validate_peripherals(data: dict, path: Path) -> None:
    peripherals = data.get("peripherals") or []
    if not isinstance(peripherals, list):
        raise ValueError(f"{path}: peripherals must be a list of mappings")
    for i, p in enumerate(peripherals):
        if not isinstance(p, dict):
            raise ValueError(f"{path}: peripheral item {i} must be a mapping")
        ptype = p.get("type")
        if not ptype:
            raise ValueError(f"{path}: peripheral at index {i} missing 'type'")
        if ptype not in _SUPPORTED_PERIPHERALS:
            raise ValueError(
                f"{path}: unrecognized peripheral type {ptype!r} at index {i}. "
                f"Supported types: {list(_SUPPORTED_PERIPHERALS.keys())}"
            )
        
        schema = _SUPPORTED_PERIPHERALS[ptype]
        for req in schema["required"]:
            if req not in p:
                raise ValueError(f"{path}: peripheral {ptype!r} at index {i} missing required key {req!r}")
        
        # Validate outputs
        if "outputs" in schema:
            outputs = p.get("outputs") or []
            if not isinstance(outputs, list):
                raise ValueError(f"{path}: peripheral outputs at index {i} must be a list of symbols")
        
        # Validate inputs
        if "inputs" in schema:
            inputs = p.get("inputs") or []
            if not isinstance(inputs, list):
                raise ValueError(f"{path}: peripheral inputs at index {i} must be a list of symbols")


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
    _validate_peripherals(data, path)


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
    profile_keys = {
        _config_key(k)
        for options in profile.values()
        if isinstance(options, dict)
        for k in options
    }
    imply_feats = (data.get("features") or {}).get("imply") or []
    feat_defaults = [f for f in imply_feats if _config_key(f) not in profile_keys]
    if feat_defaults:
        lines.append("# --- ESPD features (from features.imply) ---\n\n")
        for feat in feat_defaults:
            lines.append(_config_line(feat, True))
        lines.append("\n")
    extras = _otg_sdkconfig_extras(data, profile_keys)
    if extras:
        lines.append("# --- USB OTG console handoff (auto) ---\n\n")
        for key, value in extras:
            lines.append(_config_line(key, value))
        lines.append("\n")
    coexist = _wifi_usb_coexist_extras(data, profile_keys)
    if coexist:
        lines.append("# --- Wi-Fi + OTG (auto) ---\n\n")
        for key, value in coexist:
            lines.append(_config_line(key, value))
        lines.append("\n")
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


def _gen_peripherals_c(data: dict, src: str) -> str:
    peripherals = data.get("peripherals") or []
    if not peripherals:
        return ""
    
    init_code = []
    task_code = []
    global_code = []
    
    bsp_header = data.get("bsp", {}).get("header", "bsp/esp-bsp.h")
    
    for i, p in enumerate(peripherals):
        ptype = p["type"]
        name = p.get("name", f"device_{i}")
        
        if ptype == "sensor":
            getter = p["bsp_getter"]
            rate = p["rate_ms"]
            outputs = p["outputs"]
            
            var_declarations = ", ".join(f"val_{idx} = 0.0f" for idx in range(len(outputs)))
            var_pointers = ", ".join(f"&val_{idx}" for idx in range(len(outputs)))
            
            send_calls = []
            for idx, pd_sym in enumerate(outputs):
                send_calls.append(f'        _espd_send_pd_float("{pd_sym}", val_{idx});')
            
            send_calls_str = "\n".join(send_calls)
            
            task_code.append(f"""
        // Poll sensor {name} using BSP getter {getter}
        {{
            float {var_declarations};
            {getter}({var_pointers});
{send_calls_str}
            vTaskDelay(pdMS_TO_TICKS({rate}));
        }}
""")
            
        elif ptype == "actuator":
            setter = p["bsp_setter"]
            inputs = p["inputs"]
            
            for idx, pd_sym in enumerate(inputs):
                global_code.append(f"""
// Actuator {name} callback for index {idx}
static t_class *actuator_class_{name}_{idx};
typedef struct _actuator_recv_{name}_{idx} {{
    t_pd x_pd;
}} t_actuator_recv_{name}_{idx};
static t_actuator_recv_{name}_{idx} actuator_instance_{name}_{idx};

extern void {setter}(float val);

static void actuator_recv_val_{name}_{idx}(t_actuator_recv_{name}_{idx} *x, t_float f) {{
    {setter}((float)f);
}}
""")
                init_code.append(f"""
    // Bind Pd receiver for {pd_sym} to {setter}
    actuator_class_{name}_{idx} = class_new(gensym("_actuator_{name}_{idx}"), 0, 0, sizeof(t_actuator_recv_{name}_{idx}), CLASS_PD, 0);
    class_addfloat(actuator_class_{name}_{idx}, (t_method)actuator_recv_val_{name}_{idx});
    actuator_instance_{name}_{idx}.x_pd = actuator_class_{name}_{idx};
    pd_bind((t_pd *)&actuator_instance_{name}_{idx}, gensym("{pd_sym}"));
""")
            
    globals_str = "\n".join(global_code)
    inits_str = "\n".join(init_code)
    tasks_str = "\n".join(task_code)
    
    return f"""/*
 * Auto-generated from {src} — do not edit.
 *
 * Board custom peripherals & actuators driver task (isolated to Core 0).
 */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "{bsp_header}"
#include "../pd/src/m_pd.h"

static const char *TAG = "espd_peripherals";

static void _espd_send_pd_float(const char *name, float val)
{{
    t_symbol *sym = gensym(name);
    if (sym && sym->s_thing) {{
        pd_float(sym->s_thing, (t_float)val);
    }}
}}

{globals_str}

static void espd_peripherals_task(void *arg)
{{
    (void)arg;
    ESP_LOGI(TAG, "peripherals polling task started on Core 0");
    for (;;) {{
{tasks_str}
    }}
}}

void espd_board_peripherals_init(void)
{{
{inits_str}

    xTaskCreatePinnedToCore(espd_peripherals_task, "espd_periph", 4096, NULL, 3, NULL, 0);
}}
"""


def _generate_board(repo: Path, yaml_path: Path, boards_dir: Path) -> Path:
    try:
        rel_src = yaml_path.relative_to(repo).as_posix()
    except ValueError:
        rel_src = f"boards/{yaml_path.name}"
    data = _load_board(yaml_path)
    _validate_board(data, yaml_path)

    out_dir = repo / "components" / f"espd_board_{data['id']}"
    _write_if_changed(out_dir / "Kconfig.board", _gen_kconfig(data, rel_src))
    _write_if_changed(out_dir / "idf_component.yml", _gen_idf_component_yml(data, rel_src))
    _write_if_changed(
        out_dir / "CMakeLists.txt",
        _gen_cmake_glue(data, rel_src),
    )
    _write_if_changed(out_dir / "espd_bsp_audio_glue.c", AUDIO_GLUE_C.format(src=rel_src))
    _write_if_changed(out_dir / "espd_bsp_io_glue.c", IO_GLUE_C.format(src=rel_src))
    _write_if_changed(out_dir / "sdkconfig.defaults", _gen_sdkconfig_defaults(data, rel_src))

    io_cfg = _gen_io_config(data, rel_src)
    io_path = out_dir / "espd_board_io_config.h"
    if io_cfg:
        _write_if_changed(io_path, io_cfg)
    elif io_path.exists():
        io_path.unlink()

    periphs_c = _gen_peripherals_c(data, rel_src)
    periphs_path = out_dir / "espd_board_peripherals.c"
    if periphs_c:
        _write_if_changed(periphs_path, periphs_c)
    elif periphs_path.exists():
        periphs_path.unlink()

    return out_dir


def _parse_args(argv: list[str]) -> tuple[Path, Path]:
    repo = Path(argv[1] if len(argv) > 1 else ".").resolve()
    boards_dir = repo / "boards"
    i = 2
    while i < len(argv):
        if argv[i] == "--boards-dir" and i + 1 < len(argv):
            boards_dir = Path(argv[i + 1]).resolve()
            i += 2
            continue
        raise SystemExit(f"unknown argument: {argv[i]}")
    return repo, boards_dir


def main(argv: list[str]) -> int:
    repo, boards_dir = _parse_args(argv)
    if not boards_dir.is_dir():
        return 0

    yaml_files = sorted(
        p for p in boards_dir.glob("*.yaml") if p.name != "index.yaml"
    )
    if not yaml_files:
        return 0

    for yaml_path in yaml_files:
        out = _generate_board(repo, yaml_path, boards_dir)
        print(f"gen_board_plugins: {yaml_path} -> {out.relative_to(repo)}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
