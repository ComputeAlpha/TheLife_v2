# TheLife collector

Reports what happens on an X11 desktop to the TheLife backend: the focused window, idle time, the user's
processes, and screenshots on a schedule the backend sets (sent for analysis, never stored). Design:
`docs/collector_lld.md`.

## Build

System packages (Ubuntu 24.04):

    sudo apt install libxcb1-dev libxcb-randr0-dev libxcb-screensaver0-dev libxcb-shm0-dev libxcb-res0-dev \
      libxcb-xtest0-dev libsystemd-dev xvfb x11-xserver-utils

The rest (libcurl, jsoncpp, SQLite, libjpeg-turbo, toml++) comes from the vcpkg manifest. From the repository
root: `cmake --preset dev && cmake --build build/dev --target thelife-collector`. `-DTHELIFE_BUILD_COLLECTOR=OFF`
leaves it out.

## Use

1. Register a device in TheLife (Settings → Connections → Devices, or `POST /api/v1/devices`); it returns a
   `tld_…` token once.
2. Enroll (the token is read from standard input, never from the command line):

       thelife-collector enroll --url https://life.example.org [--web-ui https://life.example.org]

   `--web-ui` is where notification clicks open; the web UI is served by the backend, so it is usually the same URL.

3. Run it as a user service bound to the graphical session:

       install -Dm644 collector/packaging/systemd/thelife-collector.service ~/.config/systemd/user/
       systemctl --user enable --now thelife-collector

Other commands: `thelife-collector status`, `thelife-collector pause 30m` (nothing is observed until it ends),
`thelife-collector resume`.

Files: `~/.config/thelife-collector/{config.toml,device.token}` and `~/.local/state/thelife-collector/{queue.db,
state.json}`, all 0600. `config.toml` also takes `ca_file`, `pin_sha256` (`sha256//…`), `log_level` and
`args_allowlist` (executables whose arguments may be reported).

## Test

    ctest --test-dir build/dev -L collector

The system tests need Xvfb, xrandr and dbus-daemon. The e2e test also needs the test database
(`eval "$(scripts/db.sh test)"`).
