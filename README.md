# 3DS CodEdit

Purpose:
--------------------------------------------------------------------------------
3DS CodEdit is a lightweight code editor and file manager for Nintendo 3DS. This project started as a fork of 3DShell and now has its own install target, and data paths.

Current features:
--------------------------------------------------------------------------------
- Storage bar (at the very top, just beneath the current working directory).
- Precise battery percentage using mcu::hwc.
- Creating new folders and files.
- Renaming files/folders.
- File/folder deletion.
- Copy/Move files and folders.
- Multi-select items for delete/cut/copy (using Y button).
- ~~FTP server (Press select or tap the ftp icon to toggle).~~
- Image preview (If the image is around 400 * 480 which is the size of both screens, the image will be split in half and displayed. Support for the following image formats -> BMP, GIF - non animated, JPG, PCX, PNG, PGM, PPM and TGA)
- Extract various archives such as ZIP, RAR, and 7Z.
- Searching for directories (allows you to quickly visit a directory by clicking the search icon on the top right (bottom screen).)
- File properties - lets you view info on current file/folder, such as size, modified time, parent folder etc.
- ~~File timestamps~~.
- Browsing CTRNAND and copying data to/from CTRNAND.
- Dir list sorting (alphabetical - ascending, alphabetical - descending, size - largest to smallest, and size - smallest to largest).
- Online updater

Building from source (Linux):
--------------------------------------------------------------------------------
The steps below are written for Debian/Ubuntu based systems and should work in WSL as well.

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

Make sure both commands are available in your PATH:
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

Build output:
- `3DS_CodEdit.3dsx`
- `3DS_CodEdit.cia` (requires `makerom` and `bannertool`)

Troubleshooting:
- If you see `error: invalid option '--force'`, remove `--force` from old commands. Newer `dkp-pacman` versions do not support it.
- If you see `bannertool: command not found`, install it and add it to PATH, or build with explicit paths:

```bash
make BANNERTOOL=/path/to/bannertool MAKEROM=/path/to/makerom
```

- If CIA packaging fails but `3DS_CodEdit.3dsx` exists, the homebrew build succeeded and only the CIA packaging step failed.

Credits:
--------------------------------------------------------------------------------
- deltabeard/MaK11-12 for the initial ctrmus code port which was used in previous versions.
- mtheall for ftpd.
- preetisketch for the banner.
- FrozenFire for the boot logo.
