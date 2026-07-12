// Package runner provides the CommandRunner abstraction that collectors use
// to run local shell commands, so collectors can be unit tested against a
// fake runner instead of the real host.
package runner

import (
	"context"
	"os/exec"
	"time"
)

// DefaultTimeout bounds how long a single LocalRunner command is allowed to
// run. Facts collection commands (lscpu, lsblk, ip, ethtool) should all
// return in well under a second on a healthy host; a few seconds gives
// headroom without letting a hung command block a collection cycle.
const DefaultTimeout = 5 * time.Second

// CommandRunner runs a named command with arguments and returns its
// combined stdout. Collectors depend on this interface, not on os/exec
// directly, so they can be tested without a real shell.
type CommandRunner interface {
	Run(ctx context.Context, name string, args ...string) ([]byte, error)
}

// LocalRunner runs real commands on the local machine using os/exec. Every
// invocation is bounded by Timeout so a stuck command can't hang facts
// collection indefinitely.
type LocalRunner struct {
	// Timeout bounds each command. Zero means DefaultTimeout is used.
	Timeout time.Duration
}

// NewLocalRunner returns a LocalRunner using DefaultTimeout.
func NewLocalRunner() *LocalRunner {
	return &LocalRunner{Timeout: DefaultTimeout}
}

// Run executes name with args and returns its combined stdout/stderr
// output. The caller's context is combined with the runner's own timeout,
// whichever fires first wins.
func (r *LocalRunner) Run(ctx context.Context, name string, args ...string) ([]byte, error) {
	timeout := r.Timeout
	if timeout <= 0 {
		timeout = DefaultTimeout
	}

	runCtx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()

	cmd := exec.CommandContext(runCtx, name, args...)
	out, err := cmd.Output()
	if err != nil {
		if exitErr, ok := err.(*exec.ExitError); ok {
			return out, &CommandError{Name: name, Args: args, Stderr: exitErr.Stderr, Err: err}
		}
		return out, &CommandError{Name: name, Args: args, Err: err}
	}
	return out, nil
}

// CommandError wraps a failed command invocation with enough context to
// debug it without a debugger attached to the host.
type CommandError struct {
	Name   string
	Args   []string
	Stderr []byte
	Err    error
}

func (e *CommandError) Error() string {
	if len(e.Stderr) > 0 {
		return e.Name + ": " + e.Err.Error() + ": " + string(e.Stderr)
	}
	return e.Name + ": " + e.Err.Error()
}

func (e *CommandError) Unwrap() error {
	return e.Err
}
