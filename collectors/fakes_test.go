package collectors

import (
	"context"
	"errors"
	"fmt"
	"strings"
)

// fakeRunner scripts CommandRunner responses for tests. Keys are the full
// command line joined with spaces, e.g. "lspci -Dnnvmm", so a test reads as
// a list of the exact commands the collector is expected to run.
type fakeRunner struct {
	outputs map[string][]byte
	errs    map[string]error
	calls   []string
}

func newFakeRunner() *fakeRunner {
	return &fakeRunner{outputs: map[string][]byte{}, errs: map[string]error{}}
}

func (r *fakeRunner) set(cmdline string, out string) {
	r.outputs[cmdline] = []byte(out)
}

func (r *fakeRunner) setErr(cmdline string, err error) {
	r.errs[cmdline] = err
}

func (r *fakeRunner) Run(_ context.Context, name string, args ...string) ([]byte, error) {
	key := strings.Join(append([]string{name}, args...), " ")
	r.calls = append(r.calls, key)
	if err, ok := r.errs[key]; ok {
		return nil, err
	}
	if out, ok := r.outputs[key]; ok {
		return out, nil
	}
	return nil, fmt.Errorf("fakeRunner: no scripted output for %q", key)
}

// lookPathAll pretends every command is installed.
func lookPathAll(name string) (string, error) { return "/usr/bin/" + name, nil }

// lookPathNone pretends no command is installed, which is what a collector
// sees on a macOS dev machine with none of the Linux tools present.
func lookPathNone(string) (string, error) {
	return "", errors.New("not installed in test")
}
