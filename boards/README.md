# Board definitions (`boards/*.yaml`)

YAML defines **espd_board_*** plugins (generated on `idf.py` configure).

| Workflow | Where YAML lives |
|----------|------------------|
| **ESPD firmware dev** (this repo) | `espd/boards/*.yaml` (default `ESPD_BOARDS_DIR`) |
| **Product / release kits** | [espd-kits](https://github.com/ben-wes/espd-kits) `boards/` via **`ESPD_BOARDS_DIR`** at build time |

Kit builds do **not** copy YAML into this directory. Point the env var at the kits tree:

```bash
export ESPD_BOARDS_DIR=~/dev/espd/espd-kits/boards
```

Chip-wide options (dual-core layout, WL sector size, …) belong in **`sdkconfig.defaults.<target>`**, not in board YAML.

Board **selection** (`CONFIG_ESPD_BOARD_*=y`) is not set in chip defaults. Use menuconfig or **`sdkconfig.defaults.local`** (see [docs/ADDING_A_BOARD.md](../docs/ADDING_A_BOARD.md)).
