// Package collectors implements facts.Collector for real fact sources: a
// Linux host itself, a Redfish BMC, and our own provisioning store.
package collectors

import (
	"context"
	"errors"
	"os/exec"
	"strings"
	"time"

	"github.com/Rewsr/rewsr-facts/facts"
	"github.com/Rewsr/rewsr-facts/runner"
)

// LinuxHostCollector gathers CPU, disk, and NIC facts by running standard
// Linux tools (lscpu, lsblk, ip, ethtool) through a CommandRunner, plus a
// couple of direct /sys/class/net checks.
//
// This collector is also exercised on macOS dev machines where none of
// those tools exist. It never panics in that case: every command is
// checked with LookPath first, and Collect returns whatever partial facts
// it managed to gather plus a clear error describing what was missing.
type LinuxHostCollector struct {
	Runner runner.CommandRunner

	// LookPath resolves a command name to a path, or returns an error if
	// the command isn't installed. Defaults to exec.LookPath; overridable
	// in tests.
	LookPath func(file string) (string, error)
}

// NewLinuxHostCollector returns a LinuxHostCollector that runs commands
// through r and resolves them with the real exec.LookPath.
func NewLinuxHostCollector(r runner.CommandRunner) *LinuxHostCollector {
	return &LinuxHostCollector{Runner: r, LookPath: exec.LookPath}
}

// Collect gathers CPU, disk, and NIC facts. It never returns a nil
// *facts.ServerFacts: on a host missing every Linux tool it returns an
// otherwise-empty ServerFacts and a non-nil error listing what failed, so
// BuildServerFacts can still merge in facts from other collectors.
func (c *LinuxHostCollector) Collect(ctx context.Context, serverID string) (*facts.ServerFacts, error) {
	if c.Runner == nil {
		return &facts.ServerFacts{ServerID: serverID, Source: "linux_host"}, errors.New("linux host collector: no CommandRunner configured")
	}
	lookPath := c.LookPath
	if lookPath == nil {
		lookPath = exec.LookPath
	}

	f := &facts.ServerFacts{
		ServerID:   serverID,
		Source:     "linux_host",
		ObservedAt: time.Now().UTC(),
		Raw:        map[string]any{},
	}

	var errs []string

	if err := c.collectCPU(ctx, lookPath, f); err != nil {
		errs = append(errs, err.Error())
	}
	if err := c.collectDisks(ctx, lookPath, f); err != nil {
		errs = append(errs, err.Error())
	}
	if err := c.collectNICs(ctx, lookPath, f); err != nil {
		errs = append(errs, err.Error())
	}

	if len(errs) == 0 {
		return f, nil
	}
	return f, errors.New("linux host collector: " + strings.Join(errs, "; "))
}

// requireCommand checks a command is installed before we try to run it.
// This is what keeps Collect from ever shelling out to a command that
// doesn't exist and getting a confusing low-level exec error back; on a
// machine without the tool we get one clear "not installed" error instead.
func requireCommand(lookPath func(string) (string, error), name string) error {
	if _, err := lookPath(name); err != nil {
		return errors.New(name + " not found: " + err.Error())
	}
	return nil
}
