// Package api serves curated views of ServerFacts over HTTP. Handlers read
// only from a store.FactsStore, never from a facts.Collector: fact
// collection and fact serving are deliberately separate, so an API request
// never blocks on live hardware or a slow BMC.
package api

import (
	"strconv"
	"strings"

	"github.com/Rewsr/rewsr-facts/facts"
)

// PublicServer is the public, stable shape returned by GET /servers/{id}.
//
// This is a deliberate internal-vs-public contract boundary: it is built
// field by field from typed ServerFacts, and it never forwards
// ServerFacts.Raw or internal-only typed fields (NIC driver names, MAC
// addresses, rack/switch/BMC identifiers, internal VLAN IDs) to a caller.
// Those live in Raw or in typed fields that simply aren't read here.
// A caller of this API should never be able to learn, say, which switch
// port a server is on from this response, on purpose: that's operational
// detail about our fleet, not something a workload owner needs or should
// have to trust us to keep private.
type PublicServer struct {
	ServerID     string             `json:"server_id"`
	State        string             `json:"state"`
	MachineType  string             `json:"machine_type"`
	Region       string             `json:"region"`
	Network      PublicNetwork      `json:"network"`
	Capabilities PublicCapabilities `json:"capabilities"`
	Lifecycle    PublicLifecycle    `json:"lifecycle"`
}

// PublicNetwork is the network shape inside PublicServer.
type PublicNetwork struct {
	PublicIPv4      bool     `json:"public_ipv4"`
	PrivateNetworks []string `json:"private_networks"`
	BandwidthGbps   int      `json:"bandwidth_gbps"`
	Features        []string `json:"features"`
}

// PublicCapabilities is the capabilities shape inside PublicServer.
type PublicCapabilities struct {
	LocalNVMe  bool `json:"local_nvme"`
	SRIOV      bool `json:"sriov"`
	RDMA       bool `json:"rdma"`
	SecureBoot bool `json:"secure_boot"`
}

// PublicLifecycle is the lifecycle shape inside PublicServer.
type PublicLifecycle struct {
	Provisioning string `json:"provisioning"`
	Health       string `json:"health"`
	RescueMode   bool   `json:"rescue_mode"`
}

// networkFeatureCapabilities is the set of Capabilities entries that are
// safe and meaningful to surface as network-level "features" in the
// public API. This is deliberately a small allowlist rather than
// forwarding every NIC-level ethtool flag we collect: offload/checksum
// flag names are internal driver detail, not a public capability.
var networkFeatureCapabilities = map[string]bool{
	"sriov":                     true,
	"rdma":                      true,
	"high_bandwidth_networking": true,
}

// PublicView maps ServerFacts into the curated public API shape. See
// PublicServer's doc comment for why certain facts are intentionally left
// out.
func PublicView(f *facts.ServerFacts) PublicServer {
	if f == nil {
		return PublicServer{}
	}

	hasCap := func(name string) bool { return hasCapability(f.Capabilities, name) }

	var networkFeatures []string
	for _, c := range f.Capabilities {
		if networkFeatureCapabilities[c] {
			networkFeatures = append(networkFeatures, c)
		}
	}

	return PublicServer{
		ServerID:    f.ServerID,
		State:       deriveState(f),
		MachineType: deriveMachineType(f),
		Region:      f.FailureDomain,
		Network: PublicNetwork{
			PublicIPv4:      hasNetworkMode(f.NetworkModes, "public_ipv4"),
			PrivateNetworks: privateNetworks(f.NetworkModes),
			BandwidthGbps:   totalBandwidthGbps(f.NICs),
			Features:        networkFeatures,
		},
		Capabilities: PublicCapabilities{
			LocalNVMe:  hasCap("local_nvme"),
			SRIOV:      hasCap("sriov"),
			RDMA:       hasCap("rdma"),
			SecureBoot: hasCap("secure_boot"),
		},
		Lifecycle: PublicLifecycle{
			Provisioning: f.Provisioning,
			Health:       f.Health,
			// ServerFacts has no dedicated rescue-mode field: rescue
			// mode is modeled as a distinguished Provisioning value
			// rather than a second boolean that could disagree with it.
			RescueMode: strings.EqualFold(f.Provisioning, "rescue"),
		},
	}
}

// HardwareServer is the advanced-user shape returned by
// GET /servers/{id}/hardware. It exposes more detail than PublicServer
// (CPU model, per-NIC speed and features, NUMA) but still never exposes
// rack, switch, or BMC identifiers, MAC addresses, or driver names.
type HardwareServer struct {
	ServerID string        `json:"server_id"`
	CPU      HardwareCPU   `json:"cpu"`
	MemoryGB int           `json:"memory_gb"`
	NICs     []HardwareNIC `json:"nics"`
	NUMA     HardwareNUMA  `json:"numa"`
}

// HardwareCPU is the CPU shape inside HardwareServer.
type HardwareCPU struct {
	Model   string `json:"model"`
	Sockets int    `json:"sockets"`
	Cores   int    `json:"cores"`
}

// HardwareNIC is one entry in HardwareServer's nics array. It deliberately
// omits MAC address and driver name; those are internal implementation
// detail, not something an advanced user needs to pick a machine type.
type HardwareNIC struct {
	Name      string   `json:"name"`
	SpeedGbps int      `json:"speed_gbps"`
	Features  []string `json:"features"`
}

// HardwareNUMA is the numa shape inside HardwareServer.
type HardwareNUMA struct {
	Available       bool `json:"available"`
	TopologyExposed bool `json:"topology_exposed"`
}

// HardwareView maps ServerFacts into the curated hardware API shape.
func HardwareView(f *facts.ServerFacts) HardwareServer {
	if f == nil {
		return HardwareServer{}
	}

	nics := make([]HardwareNIC, 0, len(f.NICs))
	for _, n := range f.NICs {
		nics = append(nics, HardwareNIC{
			Name:      n.Name,
			SpeedGbps: n.SpeedGbps,
			Features:  n.Features,
		})
	}

	return HardwareServer{
		ServerID: f.ServerID,
		CPU: HardwareCPU{
			Model:   f.CPU.Model,
			Sockets: f.CPU.Sockets,
			Cores:   f.CPU.Cores,
		},
		MemoryGB: f.MemoryGB,
		NICs:     nics,
		NUMA:     deriveNUMA(f),
	}
}

func hasCapability(capabilities []string, name string) bool {
	for _, c := range capabilities {
		if c == name {
			return true
		}
	}
	return false
}

func hasNetworkMode(modes []string, mode string) bool {
	for _, m := range modes {
		if m == mode {
			return true
		}
	}
	return false
}

// privateNetworks returns every NetworkMode that isn't the public_ipv4
// sentinel; everything else a NIC is attached to is treated as a private
// network attachment.
func privateNetworks(modes []string) []string {
	var out []string
	for _, m := range modes {
		if m != "public_ipv4" {
			out = append(out, m)
		}
	}
	return out
}

// totalBandwidthGbps sums the speed of every observed NIC. This is a
// deliberate choice (total available bandwidth) rather than reporting the
// single fastest NIC, since bonded multi-NIC servers are common in our
// bare-metal fleet and the sum is the more useful capacity signal for a
// workload owner deciding whether a server fits their needs.
func totalBandwidthGbps(nics []facts.NICFacts) int {
	total := 0
	for _, n := range nics {
		total += n.SpeedGbps
	}
	return total
}

// deriveState summarizes provisioning/health/power into one short public
// status, distinct from the more detailed lifecycle object.
func deriveState(f *facts.ServerFacts) string {
	switch {
	case strings.EqualFold(f.Provisioning, "provisioning"):
		return "provisioning"
	case strings.EqualFold(f.Health, "healthy"):
		return "active"
	case strings.EqualFold(f.Health, "degraded"):
		return "degraded"
	default:
		return "unknown"
	}
}

// deriveMachineType synthesizes a short type string from CPU, memory, and
// disk facts, e.g. "8c-32gb-nvme". It is not raw hardware detail (no
// model name, no vendor); it is the same kind of short SKU-style string a
// cloud provider exposes for an instance type.
func deriveMachineType(f *facts.ServerFacts) string {
	if f.CPU.Cores == 0 && f.MemoryGB == 0 {
		return "unknown"
	}

	diskSuffix := ""
	switch primaryDiskType(f.Disks) {
	case "nvme":
		diskSuffix = "-nvme"
	case "ssd":
		diskSuffix = "-ssd"
	case "hdd":
		diskSuffix = "-hdd"
	}

	return strconv.Itoa(f.CPU.Cores) + "c-" + strconv.Itoa(f.MemoryGB) + "gb" + diskSuffix
}

// primaryDiskType picks the fastest disk tier present, since that's the
// tier most relevant to a machine type string (a server with one NVMe
// drive and three HDDs is still an "nvme"-class machine for this purpose).
func primaryDiskType(disks []facts.DiskFacts) string {
	best := ""
	rank := map[string]int{"hdd": 1, "ssd": 2, "nvme": 3}
	for _, d := range disks {
		if rank[d.Type] > rank[best] {
			best = d.Type
		}
	}
	return best
}

// deriveNUMA derives NUMA facts heuristically from CPU facts, since
// ServerFacts has no dedicated NUMA field yet. More than one socket
// implies more than one NUMA node; topology is considered "exposed" only
// when we actually have full socket/core/thread facts to describe it.
func deriveNUMA(f *facts.ServerFacts) HardwareNUMA {
	return HardwareNUMA{
		Available:       f.CPU.Sockets > 1,
		TopologyExposed: f.CPU.Sockets > 0 && f.CPU.Cores > 0 && f.CPU.Threads > 0,
	}
}
