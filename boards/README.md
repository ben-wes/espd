# Board definitions (`boards/*.yaml`)

YAML here defines **espd_board_*** plugins (generated on `idf.py` configure).

| Workflow | Where YAML lives |
|----------|------------------|
| **ESPD firmware dev** (this repo) | `espd/boards/*.yaml` (default) |
| **Product / release kits** | [espd-kits](https://github.com/ben-wes/espd-kits) — CI copies YAML + `sdkconfig.defaults.local` into a build tree |

Chip-wide options (dual-core layout, WL sector size, …) belong in **`sdkconfig.defaults.<target>`**, not in board YAML.

Board **selection** (`CONFIG_ESPD_BOARD_*=y`) is not set in chip defaults. Use menuconfig or **`sdkconfig.defaults.local`** (see [docs/ADDING_A_BOARD.md](../docs/ADDING_A_BOARD.md)).
