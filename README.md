# 3DS CodEdit

3DS CodEdit is a code-first editor and project workspace for Nintendo 3DS.
It combines a lightweight multi-tab text editor, project Git workflow, file manager tools, and a monochrome draw mode for quick PBM assets.

## What It Focuses On

### 1. Code editing on-device
- Multi-tab text editor (up to 4 open documents).
- Create, open, edit, save, and save-as files directly on SD.
- Line-based editing with cursor movement, line numbers, and visible cursor position (Ln/Col).
- Fast editing actions from buttons and top menu:
	- edit current line with OSK,
	- insert newline,
	- close/switch tabs,
	- find next text match,
	- undo/redo support.
- Sidebar workflow for project files: open files/folders, create new file/folder, go to parent directory.

### 2. Integrated Git + GitHub workflow
- Local Git actions:
	- init repository,
	- add all,
	- commit staged changes,
	- staged file counter and branch display.
- GitHub actions:
	- probe remote,
	- clone to current path,
	- fetch,
	- pull (fast-forward),
	- push,
	- personal access token storage.
- `.gitignore` support in add-all and push file scanning.
- Multi-repository safety improvements:
	- repository-local remote/branch state is preferred,
	- `Root*` warning when operating from a subfolder,
	- double-confirm guard for root-scope actions.

### 3. Draw mode for pixel assets (`.pbm`)
- Built-in black/white canvas editor (stylus pen + eraser).
- Create new drawings from editor sidebar (`DRAW`).
- Open existing `.pbm` files from Explorer or editor sidebar.
- Save as PBM (`P4`) and return to Git workflow for commit/push.

## File and System Features

- Explorer with storage usage bar, icons, and multi-select.
- File operations: create, rename, delete, copy, move, and batch operations.
- Archive extraction support (`.zip`, `.rar`, `.7z`, `.lzma`).
- Image viewer with zoom/pan and image properties.
- Search and quick jump to a path.
- Optional CTRNAND browsing/copying in developer options.
- Sorting modes: alphabetical (asc/desc) and size (largest/smallest).
- Dark theme, updater, and persistent config (`last_dir`, sort mode, Git defaults, token).

## Quick Navigation Notes

- Home icon toggles between Explorer and editor.
- In Explorer, `SELECT` also returns to editor.
- In editor, `SELECT` opens/closes the top menu (`File`, `Edit`, `Search`, `View`, `Project`).
- `Project` opens the Git panel.

## Building From Source (Linux)

The steps below are written for Debian/Ubuntu based systems and should also work in WSL.

1. Install devkitPro pacman (one-time setup):

```bash
sudo apt update
sudo apt install -y devkitpro-pacman
```

2. Install 3DS toolchain and required libraries:

```bash
sudo dkp-pacman -Syu --needed 3ds-dev 3ds-curl 3ds-libarchive 3ds-jansson 3ds-libjpeg-turbo 3ds-libpng --noconfirm
```

3. Configure environment variables (one-time setup):

```bash
echo 'export DEVKITPRO=/opt/devkitpro' >> ~/.bashrc
echo 'export DEVKITARM=${DEVKITPRO}/devkitARM' >> ~/.bashrc
echo 'export PATH=${DEVKITARM}/bin:${DEVKITPRO}/tools/bin:$PATH' >> ~/.bashrc
source ~/.bashrc
```

4. Install packaging tools used for CIA output:

- `makerom`: [3DSGuy/Project_CTR](https://github.com/3DSGuy/Project_CTR)
- `bannertool`: [carstene1ns/3ds-bannertool](https://github.com/carstene1ns/3ds-bannertool/releases/latest)

Verify both commands are available:

```bash
makerom --help
bannertool --help
```

5. Clone and build:

```bash
git clone --recursive https://github.com/dcuevasa/3DS_CodEdit.git
cd 3DS_CodEdit
make clean
make
```

Build outputs:

- `3DS_CodEdit.3dsx`
- `3DS_CodEdit.cia` (requires `makerom` and `bannertool`)

## Troubleshooting

- If you see `error: invalid option '--force'`, remove `--force` from old commands. Newer `dkp-pacman` versions do not support it.
- If you see `bannertool: command not found`, install it and add it to `PATH`, or build with explicit paths:

```bash
make BANNERTOOL=/path/to/bannertool MAKEROM=/path/to/makerom
```

- If CIA packaging fails but `3DS_CodEdit.3dsx` exists, the homebrew build succeeded and only the CIA packaging step failed.

## Credits

- deltabeard / MaK11-12 for the initial ctrmus code port used in previous versions.
- mtheall for ftpd.
- preetisketch for the banner.
- FrozenFire for the boot logo.

Many file-manager and platform integration features in this project are grandfathered from 3DShell.
