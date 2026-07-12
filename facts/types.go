// Package facts defines the normalized ServerFacts model shared by every
// collector, store, and API handler in rewsr-facts. Nothing in this package
// touches hardware or the network directly; it just describes the shape of
// what a collector produces and how those shapes get merged together.
package facts

import "time"

// ServerFacts is the normalized, merged view of a physical server. It is
// built by running one or more Collectors and merging their output, not by
// any single collector on its own.
type ServerFacts struct {
	ServerID     string    `json:"server_id"`
	ObservedAt   time.Time `json:"observed_at"`
	Source       string    `json:"source"`
	PowerState   string    `json:"power_state"`
	Provisioning string    `json:"provisioning"`
	Health       string    `json:"health"`

	CPU          CPUFacts    `json:"cpu"`
	MemoryGB     int         `json:"memory_gb"`
	Disks        []DiskFacts `json:"disks"`
	NICs         []NICFacts  `json:"nics"`
	Capabilities []string    `json:"capabilities"`
	NetworkModes []string    `json:"network_modes"`

	FailureDomain string `json:"failure_domain"`

	// Raw holds anything a collector observed that has not been promoted
	// to a typed field yet (vendor/model/serial number strings from
	// Redfish, for example). It round-trips through the facts store like
	// any other field so nothing is silently lost, but the API package's
	// PublicView and hardware view builders deliberately never read from
	// it: everything the public API returns is built field by field from
	// the typed facts below, never by forwarding Raw through.
	Raw map[string]any `json:"raw,omitempty"`
}

// CPUFacts describes the CPU configuration of a server.
type CPUFacts struct {
	Vendor  string `json:"vendor"`
	Model   string `json:"model"`
	Sockets int    `json:"sockets"`
	Cores   int    `json:"cores"`
	Threads int    `json:"threads"`
}

// DiskFacts describes a single block device.
type DiskFacts struct {
	Name       string `json:"name"`
	Type       string `json:"type"`
	SizeGB     int    `json:"size_gb"`
	Rotational bool   `json:"rotational"`
}

// NICFacts describes a single network interface.
type NICFacts struct {
	Name      string   `json:"name"`
	MAC       string   `json:"mac"`
	SpeedGbps int      `json:"speed_gbps"`
	Driver    string   `json:"driver"`
	Features  []string `json:"features"`
}
