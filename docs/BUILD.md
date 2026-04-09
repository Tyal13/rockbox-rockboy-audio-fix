# Building the Fixed rockboy.rock

## Prerequisites

### macOS (Apple Silicon or Intel)

```bash
brew install gnu-sed coreutils gmp mpfr libmpc isl texinfo gcc automake autoconf wget
```

### Linux (Ubuntu/Debian)

```bash
sudo apt-get install gcc g++ make libgmp-dev libmpfr-dev libmpc-dev \
    flex bison texinfo wget libncurses-dev automake autoconf
```

---

## Step 1: Clone Rockbox Source

```bash
git clone --depth=1 https://git.rockbox.org/rockbox.git
```

---

## Step 2: Build the ARM Cross-Compiler

Rockbox requires its own `arm-elf-eabi-gcc` toolchain.

```bash
cd rockbox/tools

export RBDEV_PREFIX="$HOME/rockbox-toolchain"   # space-free path required
export RBDEV_DOWNLOAD="/tmp/rbdev-dl"

# macOS: use Homebrew GCC and GNU tools
export CC=gcc-15    # adjust to your version: gcc-14, gcc-15, etc.
export CXX=g++-15
export PATH="/opt/homebrew/opt/gnu-sed/libexec/gnubin:\
/opt/homebrew/opt/coreutils/libexec/gnubin:\
/opt/homebrew/opt/texinfo/bin:$PATH"

# Select 'a' for ARM when prompted
echo "a" | bash rockboxdev.sh
```

Takes 20-40 minutes. Installs to `$HOME/rockbox-toolchain/arm-elf-eabi/`.

> **macOS note:** The install prefix must not contain spaces. Google Drive paths
> like `~/Library/CloudStorage/Google Drive/...` will break autoconf. Use a
> path under `$HOME` directly.

---

## Step 3: Apply the Patch

```bash
# From inside the cloned rockbox/ directory:
patch -p1 < /path/to/rockbox-rockboy-audio-fix/patches/rbsound.patch

# Or copy directly:
cp /path/to/rockbox-rockboy-audio-fix/src/rbsound.c \
   apps/plugins/rockboy/rbsound.c
```

---

## Step 4: Configure the Build

```bash
mkdir rockbox-build && cd rockbox-build
export PATH="$HOME/rockbox-toolchain/arm-elf-eabi/bin:$PATH"
../rockbox/tools/configure
```

When prompted:
- **Target:** `iPod Classic` (ipod6g)
- **Build type:** `N` for Normal

---

## Step 5: Build Only the Plugin

```bash
cd rockbox-build
make apps/plugins/rockboy.rock
```

Output: `apps/plugins/rockboy.rock`

---

## Step 6: Install to iPod

```bash
cp apps/plugins/rockboy.rock /Volumes/IPOD/.rockbox/rocks/games/rockboy.rock
diskutil eject /Volumes/IPOD
```

---

## Troubleshooting

| Error | Fix |
|-------|-----|
| `arm-elf-eabi-gcc: not found` | Add `$HOME/rockbox-toolchain/arm-elf-eabi/bin` to `$PATH` |
| `invalid host type` in configure | `RBDEV_PREFIX` path contains spaces (e.g. Google Drive path) -- use a space-free path like `$HOME/rockbox-toolchain` |
| `unrecognized command-line option '-fbracket-depth=512'` | `rockboxdev.sh` passes a Clang-only flag to GCC. Fix: `export CXXFLAGS="-std=gnu++03"` before running the script |
| `automake: not found` | `brew install automake autoconf` |
| `gsed: not found` | `brew install gnu-sed` |
| `greadlink: not found` | `brew install coreutils` |
| `makeinfo: not found` | `brew install texinfo` |

### macOS + Homebrew GCC 15 full setup

```bash
brew install gnu-sed coreutils gmp mpfr libmpc isl texinfo gcc automake autoconf wget

export RBDEV_PREFIX="$HOME/rockbox-toolchain"   # NO spaces in path
export CC=gcc-15
export CXX=g++-15
export CXXFLAGS="-std=gnu++03"                   # Prevent clang-only flag injection
export RBDEV_DOWNLOAD="/tmp/rbdev-dl"
export PATH="/opt/homebrew/opt/gnu-sed/libexec/gnubin:\
/opt/homebrew/opt/coreutils/libexec/gnubin:\
/opt/homebrew/opt/texinfo/bin:\
/opt/homebrew/bin:$PATH"

echo "a" | bash rockboxdev.sh
```
