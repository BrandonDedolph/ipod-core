#!/usr/bin/env bash
# Install everything needed for autonomous firmware work.
# Idempotent: safe to re-run.
#
# Languages go through asdf; system libs through apt.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GO_VERSION="1.22.10"

say() { printf "\n\033[1;36m==> %s\033[0m\n" "$*"; }
ok()  { printf "    \033[1;32m✓\033[0m %s\n" "$*"; }
warn(){ printf "    \033[1;33m!\033[0m %s\n" "$*"; }

# ---------- 1. asdf-managed languages ----------

if ! command -v asdf >/dev/null 2>&1; then
    echo "asdf not found in PATH. Source it (~/.asdf/asdf.sh) and retry." >&2
    exit 1
fi

say "Go $GO_VERSION via asdf"
if ! asdf plugin list 2>/dev/null | grep -qx golang; then
    asdf plugin add golang https://github.com/asdf-community/asdf-golang.git
    ok "added golang plugin"
else
    ok "golang plugin already present"
fi

if ! asdf list golang 2>/dev/null | grep -q "$GO_VERSION"; then
    asdf install golang "$GO_VERSION"
    ok "installed golang $GO_VERSION"
else
    ok "golang $GO_VERSION already installed"
fi

# Pin Go for the repo via .tool-versions (works with any asdf version).
TV="$REPO_ROOT/.tool-versions"
if ! grep -qx "golang $GO_VERSION" "$TV" 2>/dev/null; then
    # Replace existing golang line if present, otherwise append.
    if grep -q '^golang ' "$TV" 2>/dev/null; then
        sed -i "s|^golang .*|golang $GO_VERSION|" "$TV"
    else
        echo "golang $GO_VERSION" >> "$TV"
    fi
    ok "wrote .tool-versions"
else
    ok ".tool-versions already pins golang $GO_VERSION"
fi

# ---------- 2. system packages (distro-aware) ----------

# Detect package manager. Map our canonical needs to per-distro names.
DISTRO=""
if [[ -r /etc/os-release ]]; then
    . /etc/os-release
    DISTRO="${ID:-}"
    DISTRO_LIKE="${ID_LIKE:-}"
fi

case "$DISTRO $DISTRO_LIKE" in
    *arch*|*manjaro*|*endeavouros*)
        PM="pacman"
        PKGS=(
            # C build
            meson ninja pkgconf cmake clang
            # Sim
            sdl2
            # ARM cross-compile
            arm-none-eabi-gcc arm-none-eabi-binutils arm-none-eabi-newlib
            # USB / udev for Go CLI device detection
            libusb
            # Tooling
            git curl unzip jq github-cli
        )
        is_installed() { pacman -Q "$1" >/dev/null 2>&1; }
        install_pkgs() { sudo pacman -S --needed --noconfirm "$@"; }
        ;;
    *debian*|*ubuntu*)
        PM="apt"
        PKGS=(
            meson ninja-build pkg-config cmake clang-format
            libsdl2-dev
            gcc-arm-none-eabi binutils-arm-none-eabi libnewlib-arm-none-eabi
            libusb-1.0-0-dev libudev-dev
            git curl unzip jq gh
        )
        is_installed() { dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -q "install ok installed"; }
        install_pkgs() { sudo apt-get update -qq && sudo apt-get install -y --no-install-recommends "$@"; }
        ;;
    *fedora*|*rhel*)
        PM="dnf"
        PKGS=(
            meson ninja-build pkgconf-pkg-config cmake clang-tools-extra
            SDL2-devel
            arm-none-eabi-gcc-cs arm-none-eabi-binutils-cs arm-none-eabi-newlib
            libusbx-devel systemd-devel
            git curl unzip jq gh
        )
        is_installed() { rpm -q "$1" >/dev/null 2>&1; }
        install_pkgs() { sudo dnf install -y "$@"; }
        ;;
    *)
        echo "Unsupported distro: $DISTRO ($DISTRO_LIKE). Install these manually:" >&2
        echo "  meson ninja pkg-config cmake sdl2(dev) arm-none-eabi-{gcc,binutils,newlib} libusb(dev) jq gh" >&2
        exit 1
        ;;
esac

MISSING=()
for p in "${PKGS[@]}"; do
    if ! is_installed "$p"; then
        MISSING+=("$p")
    fi
done

if [[ ${#MISSING[@]} -eq 0 ]]; then
    say "System packages ($PM): all present"
else
    say "Installing ${#MISSING[@]} packages via $PM"
    printf "    %s\n" "${MISSING[@]}"
    install_pkgs "${MISSING[@]}"
fi

# ---------- 3. verification ----------

say "Verifying"
cd "$REPO_ROOT"  # so asdf picks up .tool-versions for the go check
ok "go             $(go version | awk '{print $3}')"
ok "meson          $(meson --version)"
ok "ninja          $(ninja --version)"
ok "arm-none-eabi  $(arm-none-eabi-gcc --version | head -1 | awk '{print $NF}')"
ok "sdl2           $(pkg-config --modversion sdl2)"
ok "gh             $(gh --version 2>/dev/null | head -1 || echo 'not installed (run: sudo apt install gh)')"

say "Ready"
echo "    Repo:    $REPO_ROOT"
echo "    Go:      $GO_VERSION (pinned via .tool-versions)"
echo "    ARM:     $(arm-none-eabi-gcc -dumpversion)"
echo
echo "Start me with --dangerously-skip-permissions for unattended work."
