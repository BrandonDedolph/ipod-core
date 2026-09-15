// Command core-app is the desktop application for the custom iPod
// firmware project: one window over the same internal packages the
// `core` CLI drives.
//
// It is a second executable rather than a `core ui` subcommand for one
// reason, and it is a Windows reason: an exe linked with
// `-H windowsgui` has no console at all, so `core sync` typed in a
// terminal would print nothing; an exe linked without it flashes a
// black console every time the app starts. Two files avoid both. The
// two binaries ship side by side, and the app runs the CLI beside it as
// the elevated child when a flash needs Administrator — one write path,
// and a UAC prompt naming the binary the user was told does the writing.
//
// Usage:
//
//	core-app                          open the window
//	core-app --source DIR             open it with the music folder preset
//	core-app --log FILE               append startup diagnostics to FILE
//	core-app --screenshot OUT.png [--state demo|empty] [--size 900x600]
//	                                  render one frame headless and exit
//
// --screenshot is the review channel: a `-H windowsgui` process cannot
// show anyone anything from a shell, and this writes the exact frame
// the window would draw to a file that can be looked at.
package main

import (
	"flag"
	"fmt"
	"os"
	"strconv"
	"strings"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/app"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/version"
)

func main() {
	var (
		screenshot  = flag.String("screenshot", "", "Render one frame to this PNG and exit (no window)")
		stateName   = flag.String("state", "demo", "Which canned state --screenshot renders: demo, empty")
		size        = flag.String("size", "900x600", "--screenshot size, WxH")
		source      = flag.String("source", "", "Preset the music source folder")
		logFile     = flag.String("log", "", "Append startup diagnostics to this file")
		showVersion = flag.Bool("version", false, "Print the version and exit")
	)
	flag.Parse()

	if *showVersion {
		fmt.Println("core-app", version.Full())
		return
	}

	if *screenshot != "" {
		w, h, err := parseSize(*size)
		if err != nil {
			fail(*logFile, err)
		}
		st, err := cannedState(*stateName)
		if err != nil {
			fail(*logFile, err)
		}
		if err := app.Snapshot(st, w, h, *screenshot); err != nil {
			fail(*logFile, err)
		}
		fmt.Printf("wrote %s (%dx%d, state %s)\n", *screenshot, w, h, *stateName)
		return
	}

	// The config is read before the window so a failure to read it is a
	// line in --log and not a window that opens with the source box
	// mysteriously empty.
	cfgPath, cfgErr := app.ConfigPath()
	var cfg app.Config
	if cfgErr == nil {
		cfg, cfgErr = app.LoadConfig(cfgPath)
	}

	o := app.Options{
		Backend:    app.NewRealBackend(cfg.BackupDir),
		Config:     cfg,
		ConfigPath: cfgPath,
		Source:     *source,
		LogFile:    *logFile,
	}
	if cfgErr != nil {
		writeLog(*logFile, "config: %v", cfgErr)
	}
	writeLog(*logFile, "core-app %s starting", version.Full())
	if err := app.Main(o); err != nil {
		fail(*logFile, err)
	}
}

func cannedState(name string) (app.State, error) {
	switch strings.ToLower(strings.TrimSpace(name)) {
	case "demo", "":
		return app.DemoState(), nil
	case "empty", "nodevice":
		return app.EmptyState(), nil
	default:
		return app.State{}, fmt.Errorf("--state %q: expected demo or empty", name)
	}
}

func parseSize(s string) (int, int, error) {
	parts := strings.SplitN(strings.ToLower(strings.TrimSpace(s)), "x", 2)
	if len(parts) != 2 {
		return 0, 0, fmt.Errorf("--size %q: expected WxH, e.g. 900x600", s)
	}
	w, err1 := strconv.Atoi(parts[0])
	h, err2 := strconv.Atoi(parts[1])
	if err1 != nil || err2 != nil || w < 200 || h < 200 {
		return 0, 0, fmt.Errorf("--size %q: expected WxH with both at least 200", s)
	}
	return w, h, nil
}

// fail is the only exit path for an error. It writes to --log as well
// as stderr because on Windows this binary has no console: stderr goes
// nowhere and the log file is the only evidence that survives.
func fail(logPath string, err error) {
	writeLog(logPath, "core-app: %v", err)
	fmt.Fprintln(os.Stderr, "core-app:", err)
	os.Exit(1)
}

func writeLog(path, format string, args ...any) {
	if path == "" {
		return
	}
	f, ferr := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_APPEND, 0o644)
	if ferr != nil {
		return
	}
	defer f.Close()
	fmt.Fprintf(f, format+"\n", args...)
}
