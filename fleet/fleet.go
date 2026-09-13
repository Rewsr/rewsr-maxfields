// Package fleet reduces a set of per-server facts to the fleet-level
// answers operators actually ask: how much capacity exists, which
// capabilities are where, how firmware versions are spread, and which
// servers can host a workload with given requirements. Everything here is
// pure computation over already-collected facts; nothing touches the
// store or the network.
package fleet

import (
	"encoding/json"
	"sort"

	"github.com/Rewsr/rewsr-maxfields/facts"
)

// Summary is the fleet rollup.
type Summary struct {
	Servers int `json:"servers"`

	Healthy  int `json:"healthy"`
	Degraded int `json:"degraded"`
	Unknown  int `json:"unknown"`

	PoweredOn int `json:"powered_on"`

	TotalCores    int `json:"total_cores"`
	TotalThreads  int `json:"total_threads"`
	TotalMemoryGB int `json:"total_memory_gb"`
	TotalDiskGB   int `json:"total_disk_gb"`
	TotalNICGbps  int `json:"total_nic_gbps"`

	// CapabilityCounts maps each capability to how many servers hold it.
	CapabilityCounts map[string]int `json:"capability_counts,omitempty"`

	// NetworkModeCounts maps each network mode to its server count.
	NetworkModeCounts map[string]int `json:"network_mode_counts,omitempty"`

	// NICSpeedPorts maps a port speed in Gbps to how many ports run at
	// it across the fleet.
	NICSpeedPorts map[int]int `json:"nic_speed_ports,omitempty"`

	// CPUModels maps CPU model strings to server counts.
	CPUModels map[string]int `json:"cpu_models,omitempty"`

	// FailureDomains groups server ids by their failure domain; servers
	// with no recorded domain land under "unassigned".
	FailureDomains map[string][]string `json:"failure_domains,omitempty"`
}

// Aggregate reduces the given facts to a Summary. Nil entries are
// skipped, so callers can pass a store result straight through.
func Aggregate(all []*facts.ServerFacts) Summary {
	s := Summary{
		CapabilityCounts:  map[string]int{},
		NetworkModeCounts: map[string]int{},
		NICSpeedPorts:     map[int]int{},
		CPUModels:         map[string]int{},
		FailureDomains:    map[string][]string{},
	}

	for _, f := range all {
		if f == nil {
			continue
		}
		s.Servers++

		switch f.Health {
		case "healthy":
			s.Healthy++
		case "degraded":
			s.Degraded++
		default:
			s.Unknown++
		}
		if f.PowerState == "on" {
			s.PoweredOn++
		}

		s.TotalCores += f.CPU.Cores
		s.TotalThreads += f.CPU.Threads
		s.TotalMemoryGB += f.MemoryGB
		for _, d := range f.Disks {
			s.TotalDiskGB += d.SizeGB
		}
		for _, n := range f.NICs {
			s.TotalNICGbps += n.SpeedGbps
			if n.SpeedGbps > 0 {
				s.NICSpeedPorts[n.SpeedGbps]++
			}
		}

		for _, c := range f.Capabilities {
			s.CapabilityCounts[c]++
		}
		for _, m := range f.NetworkModes {
			s.NetworkModeCounts[m]++
		}
		if f.CPU.Model != "" {
			s.CPUModels[f.CPU.Model]++
		}

		domain := f.FailureDomain
		if domain == "" {
			domain = "unassigned"
		}
		s.FailureDomains[domain] = append(s.FailureDomains[domain], f.ServerID)
	}

	for _, ids := range s.FailureDomains {
		sort.Strings(ids)
	}
	return s
}

// FindCapable returns the ids of servers holding every required
// capability, sorted. An empty requirement matches every server.
func FindCapable(all []*facts.ServerFacts, required ...string) []string {
	var ids []string
	for _, f := range all {
		if f == nil {
			continue
		}
		have := make(map[string]bool, len(f.Capabilities))
		for _, c := range f.Capabilities {
			have[c] = true
		}
		ok := true
		for _, r := range required {
			if !have[r] {
				ok = false
				break
			}
		}
		if ok {
			ids = append(ids, f.ServerID)
		}
	}
	sort.Strings(ids)
	return ids
}

// FirmwareSpread maps component identity to firmware version to server
// count: the "which of our three PM9A3 firmware revisions is this fleet
// actually on" answer. It reads the nvme and nic_offload sections of Raw,
// tolerating both the in-process typed values and the generic maps those
// values become after a JSON round trip through the store.
func FirmwareSpread(all []*facts.ServerFacts) map[string]map[string]int {
	spread := map[string]map[string]int{}
	bump := func(component, version string) {
		if component == "" || version == "" {
			return
		}
		if spread[component] == nil {
			spread[component] = map[string]int{}
		}
		spread[component][version]++
	}

	for _, f := range all {
		if f == nil || f.Raw == nil {
			continue
		}

		var nvmes []struct {
			Model    string `json:"model"`
			Firmware string `json:"firmware"`
		}
		if decodeRaw(f.Raw["nvme"], &nvmes) {
			// A server with four identical drives on one firmware is one
			// data point per distinct (model, firmware), not four.
			seen := map[[2]string]bool{}
			for _, d := range nvmes {
				key := [2]string{d.Model, d.Firmware}
				if !seen[key] {
					seen[key] = true
					bump("nvme:"+d.Model, d.Firmware)
				}
			}
		}

		var nics map[string]struct {
			Driver map[string]string `json:"driver"`
		}
		if decodeRaw(f.Raw["nic_offload"], &nics) {
			seen := map[[2]string]bool{}
			for _, n := range nics {
				driver := n.Driver["driver"]
				fw := n.Driver["firmware-version"]
				key := [2]string{driver, fw}
				if !seen[key] {
					seen[key] = true
					bump("nic:"+driver, fw)
				}
			}
		}
	}
	return spread
}

// decodeRaw converts a Raw entry into out via a JSON round trip, which
// makes typed in-process values and store-round-tripped generic maps look
// identical to the caller. Returns false when the entry is absent or has
// an incompatible shape.
func decodeRaw(v any, out any) bool {
	if v == nil {
		return false
	}
	data, err := json.Marshal(v)
	if err != nil {
		return false
	}
	return json.Unmarshal(data, out) == nil
}
