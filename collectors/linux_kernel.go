package collectors

import (
	"context"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"

	"github.com/Rewsr/rewsr-facts/facts"
	"github.com/Rewsr/rewsr-facts/runner"
)

// KernelCollector records the kernel-side facts a fabric data plane
// depends on: the release, the boot command line, whether the modules
// that matter (vfio, RDMA core, the mlx5 pair, kvm) are actually loaded,
// and the handful of net.core sysctls that gate busy polling and socket
// buffer ceilings. These are the settings that make two "identical"
// servers behave differently, so they are collected instead of assumed.
type KernelCollector struct {
	Runner runner.CommandRunner

	// LookPath resolves a command name to a path, or errors if the
	// command isn't installed. Defaults to exec.LookPath; overridable in
	// tests.
	LookPath func(file string) (string, error)

	// SysRoot is prepended to every /proc path this collector reads. The
	// zero value means the real filesystem root; tests point it at a
	// fixture tree instead.
	SysRoot string
}

// NewKernelCollector returns a KernelCollector running commands through r.
func NewKernelCollector(r runner.CommandRunner) *KernelCollector {
	return &KernelCollector{Runner: r, LookPath: exec.LookPath}
}

func (c *KernelCollector) root() string {
	if c.SysRoot == "" {
		return "/"
	}
	return c.SysRoot
}

// interestingModules are the loaded-module names worth reporting. The
// list is deliberately short: it answers "can this host do passthrough,
// RDMA, and virtualization right now", not "what is loaded".
var interestingModules = []string{
	"vfio",
	"vfio_pci",
	"ib_core",
	"ib_uverbs",
	"rdma_ucm",
	"mlx5_core",
	"mlx5_ib",
	"kvm_amd",
	"kvm_intel",
	"bonding",
	"openvswitch",
	"wireguard",
}

// interestingSysctls are read from /proc/sys; the key doubles as the
// reported name with dots restored.
var interestingSysctls = []string{
	"net/core/busy_poll",
	"net/core/busy_read",
	"net/core/rmem_max",
	"net/core/wmem_max",
	"net/core/netdev_max_backlog",
}

// Collect gathers kernel facts. Sources degrade independently; only a
// host where nothing was readable at all reports an error.
func (c *KernelCollector) Collect(ctx context.Context, serverID string) (*facts.ServerFacts, error) {
	f := &facts.ServerFacts{
		ServerID:   serverID,
		Source:     "linux_kernel",
		ObservedAt: time.Now().UTC(),
		Raw:        map[string]any{},
	}

	lookPath := c.LookPath
	if lookPath == nil {
		lookPath = exec.LookPath
	}

	raw := map[string]any{}
	sawAnySource := false

	if c.Runner != nil && requireCommand(lookPath, "uname") == nil {
		if out, err := c.Runner.Run(ctx, "uname", "-r"); err == nil {
			raw["release"] = strings.TrimSpace(string(out))
			sawAnySource = true
		}
	}

	if data, err := os.ReadFile(filepath.Join(c.root(), "proc/cmdline")); err == nil {
		raw["cmdline"] = strings.TrimSpace(string(data))
		sawAnySource = true
	}

	if data, err := os.ReadFile(filepath.Join(c.root(), "proc/modules")); err == nil {
		sawAnySource = true
		loaded := parseLoadedModules(data)
		var present []string
		for _, m := range interestingModules {
			if loaded[m] {
				present = append(present, m)
			}
		}
		if len(present) > 0 {
			raw["modules"] = present
		}
	}

	sysctls := map[string]string{}
	for _, s := range interestingSysctls {
		if data, err := os.ReadFile(filepath.Join(c.root(), "proc/sys", s)); err == nil {
			sysctls[strings.ReplaceAll(s, "/", ".")] = strings.TrimSpace(string(data))
			sawAnySource = true
		}
	}
	if len(sysctls) > 0 {
		raw["sysctls"] = sysctls
	}

	if !sawAnySource {
		return f, errors.New("kernel collector: no kernel sources readable under " + c.root())
	}

	f.Raw["kernel"] = raw

	if modules, ok := raw["modules"].([]string); ok {
		for _, m := range modules {
			if m == "vfio_pci" {
				f.Capabilities = []string{"vfio"}
				break
			}
		}
	}

	return f, nil
}

// parseLoadedModules parses /proc/modules, whose lines start with the
// module name followed by size and refcount fields.
func parseLoadedModules(data []byte) map[string]bool {
	loaded := map[string]bool{}
	for _, line := range strings.Split(string(data), "\n") {
		name, _, ok := strings.Cut(line, " ")
		if ok && name != "" {
			loaded[name] = true
		}
	}
	return loaded
}
