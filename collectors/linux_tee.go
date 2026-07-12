package collectors

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/Rewsr/rewsr-facts/facts"
)

// TEECollector detects hardware trusted-execution capability on the host:
// AMD SEV, SEV-ES, and SEV-SNP, Intel TDX, and Intel SGX. Rewsr schedules
// confidential workloads onto bare metal, so "which TEE can this server
// actually run" is a first-class fact rather than something inferred from
// a CPU model string.
//
// Everything is read from /proc and /sys, no external tools. Each source
// is optional on its own; the collector only errors when it could read
// nothing at all (a non-Linux dev machine, typically), and even then it
// returns empty facts rather than nil, per the Collector contract.
type TEECollector struct {
	// SysRoot is prepended to every path this collector reads. The zero
	// value means the real filesystem root; tests point it at a fixture
	// tree instead.
	SysRoot string
}

// NewTEECollector returns a TEECollector reading from the real root.
func NewTEECollector() *TEECollector {
	return &TEECollector{}
}

func (c *TEECollector) root() string {
	if c.SysRoot == "" {
		return "/"
	}
	return c.SysRoot
}

// Collect gathers TEE capability facts. Capability strings are only added
// for features that are actually present and enabled as far as the host
// exposes them, never for "the CPU family could support this in theory".
func (c *TEECollector) Collect(ctx context.Context, serverID string) (*facts.ServerFacts, error) {
	f := &facts.ServerFacts{
		ServerID:   serverID,
		Source:     "linux_tee",
		ObservedAt: time.Now().UTC(),
		Raw:        map[string]any{},
	}

	sawAnySource := false

	flags := map[string]bool{}
	if data, err := os.ReadFile(filepath.Join(c.root(), "proc/cpuinfo")); err == nil {
		sawAnySource = true
		flags = cpuinfoFlagSet(data)
	}

	// kvm_amd module parameters report whether the kernel actually turned
	// each SEV generation on, which is stricter than the CPU merely
	// advertising the flag (firmware, BIOS settings, or kernel config can
	// all disable it).
	kvmParams := map[string]bool{}
	for _, p := range []string{"sev", "sev_es", "sev_snp"} {
		path := filepath.Join(c.root(), "sys/module/kvm_amd/parameters", p)
		if v, err := os.ReadFile(path); err == nil {
			sawAnySource = true
			kvmParams[p] = parseBoolParam(string(v))
		}
	}
	if v, err := os.ReadFile(filepath.Join(c.root(), "sys/module/kvm_intel/parameters/tdx")); err == nil {
		sawAnySource = true
		kvmParams["tdx"] = parseBoolParam(string(v))
	}

	devices := map[string]bool{}
	for _, d := range []string{"dev/sev", "dev/sev-guest", "dev/tdx_guest", "dev/sgx_enclave"} {
		if _, err := os.Stat(filepath.Join(c.root(), d)); err == nil {
			sawAnySource = true
			devices["/"+d] = true
		}
	}

	if !sawAnySource {
		return f, errors.New("tee collector: no TEE-related sources readable under " + c.root())
	}

	var caps []string
	addCap := func(name string, present bool) {
		if present {
			caps = append(caps, name)
		}
	}

	// SEV generations. The cpuinfo flag means the hardware and firmware
	// advertise it; the kvm_amd parameter means the kernel enabled it. We
	// require the flag and treat an explicitly-false kvm parameter as
	// disabled, so a BIOS-disabled SEV box does not get scheduled as
	// SEV-capable.
	sevEnabled := func(flag, param string) bool {
		if !flags[flag] {
			return false
		}
		if v, ok := kvmParams[param]; ok {
			return v
		}
		return true
	}
	addCap("sev", sevEnabled("sev", "sev"))
	addCap("sev_es", sevEnabled("sev_es", "sev_es"))
	addCap("sev_snp", sevEnabled("sev_snp", "sev_snp"))

	addCap("tdx", kvmParams["tdx"] || flags["tdx_guest"] || devices["/dev/tdx_guest"])
	addCap("sgx", flags["sgx"])

	if len(caps) > 0 {
		caps = append(caps, "tee_capable")
	}
	f.Capabilities = caps

	teeRaw := map[string]any{}
	if len(flags) > 0 {
		var teeFlags []string
		for _, name := range []string{"sme", "sev", "sev_es", "sev_snp", "tdx_guest", "sgx", "sgx_lc"} {
			if flags[name] {
				teeFlags = append(teeFlags, name)
			}
		}
		teeRaw["cpu_flags"] = teeFlags
	}
	if len(kvmParams) > 0 {
		teeRaw["kvm_params"] = kvmParams
	}
	if len(devices) > 0 {
		var present []string
		for d := range devices {
			present = append(present, d)
		}
		teeRaw["devices"] = present
	}
	f.Raw["tee"] = teeRaw

	return f, nil
}

// cpuinfoFlagSet parses the first "flags" line of /proc/cpuinfo into a set.
// Every logical CPU repeats the same flags line, so the first one is enough.
func cpuinfoFlagSet(cpuinfo []byte) map[string]bool {
	set := map[string]bool{}
	for _, line := range strings.Split(string(cpuinfo), "\n") {
		name, value, ok := strings.Cut(line, ":")
		if !ok || strings.TrimSpace(name) != "flags" {
			continue
		}
		for _, flag := range strings.Fields(value) {
			set[flag] = true
		}
		break
	}
	return set
}

// parseBoolParam interprets a kernel module parameter value. kvm_amd
// exposes these as "Y"/"N" on most kernels and "1"/"0" on some.
func parseBoolParam(v string) bool {
	switch strings.TrimSpace(v) {
	case "Y", "y", "1":
		return true
	}
	return false
}
