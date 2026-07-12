package facts

import (
	"sort"
	"strings"
)

// InferCapabilities derives the Capabilities slice from merged facts. It
// starts from whatever capability strings a collector already contributed
// (a Redfish endpoint might report "secure_boot", for example) and adds
// capabilities that can be derived directly from other facts, so callers
// never have to hardcode a capability list per server.
func InferCapabilities(f *ServerFacts) []string {
	if f == nil {
		return nil
	}

	seen := make(map[string]bool)
	var out []string
	add := func(cap string) {
		if cap == "" || seen[cap] {
			return
		}
		seen[cap] = true
		out = append(out, cap)
	}

	for _, c := range f.Capabilities {
		add(c)
	}

	for _, d := range f.Disks {
		if strings.EqualFold(d.Type, "nvme") {
			add("local_nvme")
			break
		}
	}

	for _, n := range f.NICs {
		if n.SpeedGbps >= 25 {
			add("high_bandwidth_networking")
		}
		for _, feat := range n.Features {
			switch strings.ToLower(feat) {
			case "sriov", "sr-iov":
				add("sriov")
			case "rdma":
				add("rdma")
			}
		}
	}

	if f.PowerState != "" {
		add("power_control")
	}

	sort.Strings(out)
	return out
}

// InferHealth derives a coarse Health value from merged facts rather than
// trusting any single collector's opinion. It is deliberately conservative:
// anything short of "we have real hardware facts and the server is powered
// on" is reported as degraded or unknown instead of healthy.
func InferHealth(f *ServerFacts) string {
	if f == nil {
		return "unknown"
	}

	hasHardwareFacts := f.CPU.Cores > 0 || f.MemoryGB > 0 || len(f.Disks) > 0 || len(f.NICs) > 0

	if !hasHardwareFacts && f.PowerState == "" && f.Provisioning == "" {
		// No collector produced anything usable.
		return "unknown"
	}

	if strings.EqualFold(f.Provisioning, "failed") || strings.EqualFold(f.Provisioning, "error") {
		return "degraded"
	}

	if f.PowerState != "" && !strings.EqualFold(f.PowerState, "on") {
		// We know the power state and it is not "on": that is a real
		// degraded signal, not just missing data.
		return "degraded"
	}

	if !hasHardwareFacts {
		// We know the server is provisioned and/or powered on but never
		// got real hardware facts back (host collector unreachable, for
		// example). Don't call that healthy.
		return "degraded"
	}

	return "healthy"
}
