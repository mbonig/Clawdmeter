#!/bin/sh
# Clawdmeter host daemon — one-line installer for macOS and Linux.
#
#   curl -fsSL https://raw.githubusercontent.com/mbonig/Clawdmeter/main/install-daemon.sh | sh
#
# Fetches this repo to a permanent location, then hands off to the platform
# installer that already lives in it: install-mac.sh (launchd LaunchAgent) or
# install.sh (systemd user unit). The checkout has to stick around — both
# installers point their service file at the daemon's absolute path in it.
#
# This only installs the *host* daemon. Flashing firmware to the board still
# needs PlatformIO and a USB cable; see the README.
#
# Overrides:
#   CLAWDMETER_DIR=<path>   where to install   (default ~/.local/share/clawdmeter)
#   CLAWDMETER_REF=<ref>    branch or tag      (default main)
set -eu

REPO_URL="https://github.com/mbonig/Clawdmeter"
DIR="${CLAWDMETER_DIR:-$HOME/.local/share/clawdmeter}"
REF="${CLAWDMETER_REF:-main}"

die() { printf 'error: %s\n' "$1" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

case "$(uname -s)" in
    Darwin) INSTALLER="install-mac.sh" ;;
    Linux)  INSTALLER="install.sh" ;;
    *) die "unsupported OS '$(uname -s)'. On Windows run install-windows.ps1 from a clone." ;;
esac

have curl || die "curl is required"
have bash || die "bash is required"

# ---------------------------------------------------------------- fetch source
printf '==> Fetching Clawdmeter (%s) into %s\n' "$REF" "$DIR"
mkdir -p "$(dirname "$DIR")"

if [ -d "$DIR/.git" ] && have git; then
    # Existing checkout: fast-forward only. Never reset --hard — someone may
    # have edited their config or be running a local branch here.
    git -C "$DIR" fetch --quiet origin "$REF"
    git -C "$DIR" checkout --quiet "$REF" 2>/dev/null || true
    if ! git -C "$DIR" merge --ff-only --quiet "origin/$REF" 2>/dev/null; then
        echo "    note: could not fast-forward (local changes?) — installing from the checkout as-is"
    fi
elif have git; then
    git clone --quiet --depth 1 --branch "$REF" "$REPO_URL.git" "$DIR"
else
    # No git (and on macOS invoking it would pop the Xcode CLT prompt) — take
    # the tarball. Extracted over the top of any existing dir, which leaves
    # daemon/.venv and your config in place.
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    curl -fsSL "$REPO_URL/archive/refs/heads/$REF.tar.gz" \
        | tar -xzf - -C "$tmp" --strip-components=1
    mkdir -p "$DIR"
    cp -R "$tmp/." "$DIR/"
fi

[ -f "$DIR/$INSTALLER" ] || die "$INSTALLER missing from $DIR — is '$REF' a valid ref?"
chmod +x "$DIR/$INSTALLER"

# --------------------------------------------------------- run the real thing
# stdin is the piped script under `curl | sh`, so reconnect it to the terminal
# if there is one. The platform installers ask a few questions and skip them
# when stdin isn't a TTY, so a headless run still completes with defaults.
printf '==> Running %s\n\n' "$INSTALLER"
# Test by actually opening it: `[ -r /dev/tty ]` is an access(2) check that
# succeeds even in a session with no controlling terminal, where the open then
# fails with ENXIO and takes the installer down with it.
if { exec 3</dev/tty; } 2>/dev/null; then
    exec 3<&-
    bash "$DIR/$INSTALLER" < /dev/tty
else
    echo "    (no terminal available — using defaults for every prompt)"
    bash "$DIR/$INSTALLER" < /dev/null
fi

printf '\nInstalled from: %s\n' "$DIR"
printf 'Re-run this command any time to update.\n'
