# Development Sandbox

Mock data for local TeddyCloud development. Start a local server to see behavior and try out features.

**Source** for `dev-sandbox/run/` (gitignored). Uses Docker when available; in devcontainer runs natively.

## Contents

- **20 tonies**: All custom (custom_001 … custom_020), each with custom audio
- **20 models**: custom_001 … custom_020 in `tonies.custom.json`
- **20 TAF files**: `library/audio_001.taf` … `audio_020.taf`
- **20 images**: `custom_img/custom_001.png` … `custom_020.png`
- **1 Toniebox**: Mock box "Test Toniebox" (shows in Tonieboxes list)


## Usage

```bash
make dev-sandbox-up     # Start (auto-setup on first run)
# Open http://localhost
make dev-sandbox-down
```

- **Docker:** Bind mounts `dev-sandbox/run/` to `/teddycloud`, builds local image.
- **Native:** Runs `bin/teddycloud` with `--base_path dev-sandbox/run/`.

Server certificates are generated once on first run (or when missing) and reused. No RSA regeneration on each start.

After code changes: `make dev-sandbox-restart`. To reset: `make dev-sandbox-setup`.
