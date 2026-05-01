# `core` — host CLI

Single Go binary that handles install, update, recovery, dev iteration,
the simulator, and the test suite. One executable per platform; users
download `core-{os}-{arch}` from GitHub Releases and run it.

## Build

```bash
cd core/cli
go build -o core ./cmd/core
./core --help
```

Cross-compile for other platforms:

```bash
GOOS=darwin  GOARCH=amd64 go build -o core-darwin-amd64  ./cmd/core
GOOS=darwin  GOARCH=arm64 go build -o core-darwin-arm64  ./cmd/core
GOOS=linux   GOARCH=amd64 go build -o core-linux-amd64   ./cmd/core
GOOS=linux   GOARCH=arm64 go build -o core-linux-arm64   ./cmd/core
GOOS=windows GOARCH=amd64 go build -o core-windows.exe   ./cmd/core
```

## Test

```bash
cd core/cli
go test ./...
```

## Layout

```
cmd/core/         main package (just calls cli.Root().Execute())
internal/
  cli/            cobra command tree (one file per subcommand)
  firmware/       iPod firmware-partition format helpers (checksum, image header)
  ipod/           USB device detection (per-platform, currently stubbed)
  version/        build version info, stamped via -ldflags at release time
```

## Status

This PR scaffolds the command tree and lands the parts that don't need
hardware in the loop:

- All 10 subcommands (`build`, `sim`, `flash`, `install`, `update`,
  `recover`, `info`, `debug`, `test`, `release`) are wired with help
  text and flags. Most return `not yet implemented` for now.
- `internal/firmware` has the iPod-partition checksum and directory-
  entry codec, with passing unit tests (verified against the format
  spec in `core/docs/hw/08-boot-dock.md`).
- `internal/ipod` is scaffolded with the type system (Generation,
  Mode, Device); the actual USB enumeration is stubbed and lands in a
  later PR with per-platform code.

## Design notes

- **Cobra** for the command tree. We considered urfave/cli too — cobra
  won on better help-text rendering and richer flag types.
- **No `fatih/color` etc.** — kept the dep tree to cobra + pflag +
  mousetrap (cobra's transitive Windows-mode dep). Anything more goes
  through a code review.
- The CLI is the *only* thing that talks to GitHub. Shipping anything
  unsigned or fetching anything insecure would be a security blunder
  in this kind of tool, so signature verification (in `internal/release`)
  will be a hard requirement when `core update` lands.
