package collectors

import (
	"context"
	"encoding/json"
	"errors"
	"os/exec"
	"strings"
	"time"

	"github.com/Rewsr/rewsr-maxfields/facts"
	"github.com/Rewsr/rewsr-maxfields/runner"
)

// NICOffloadCollector goes one level deeper than the basic NIC scan: per
// interface it records the driver and firmware (ethtool -i), ring sizes
// (ethtool -g), channel counts (ethtool -l), and hardware timestamping
// support (ethtool -T). Ring and channel headroom decide how far a host
// can scale packet processing without touching hardware, and a PTP
// hardware clock is what makes cross-host latency numbers trustworthy, so
// all of it is fleet-level fact material.
type NICOffloadCollector struct {
	Runner runner.CommandRunner

	// LookPath resolves a command name to a path, or errors if the
	// command isn't installed. Defaults to exec.LookPath; overridable in
	// tests.
	LookPath func(file string) (string, error)
}

// NewNICOffloadCollector returns a NICOffloadCollector running commands
// through r.
func NewNICOffloadCollector(r runner.CommandRunner) *NICOffloadCollector {
	return &NICOffloadCollector{Runner: r, LookPath: exec.LookPath}
}

// nicOffloadFacts is the per-interface entry carried in Raw["nic_offload"].
type nicOffloadFacts struct {
	Driver          map[string]string `json:"driver,omitempty"`
	RingsMax        map[string]int    `json:"rings_max,omitempty"`
	RingsCurrent    map[string]int    `json:"rings_current,omitempty"`
	ChannelsMax     map[string]int    `json:"channels_max,omitempty"`
	ChannelsCurrent map[string]int    `json:"channels_current,omitempty"`
	TimestampCaps   []string          `json:"timestamp_caps,omitempty"`
	PHCIndex        int               `json:"phc_index"`
}

// Collect gathers deep NIC facts for every ethernet interface. Individual
// ethtool subcommands failing for one interface (virtual functions
// commonly reject -g) just leave that section empty; only the absence of
// the tools themselves is an error.
func (c *NICOffloadCollector) Collect(ctx context.Context, serverID string) (*facts.ServerFacts, error) {
	f := &facts.ServerFacts{
		ServerID:   serverID,
		Source:     "linux_nic_offload",
		ObservedAt: time.Now().UTC(),
		Raw:        map[string]any{},
	}

	if c.Runner == nil {
		return f, errors.New("nic offload collector: no CommandRunner configured")
	}
	lookPath := c.LookPath
	if lookPath == nil {
		lookPath = exec.LookPath
	}
	if err := requireCommand(lookPath, "ip"); err != nil {
		return f, errors.New("nic offload collector: " + err.Error())
	}
	if err := requireCommand(lookPath, "ethtool"); err != nil {
		return f, errors.New("nic offload collector: " + err.Error())
	}

	out, err := c.Runner.Run(ctx, "ip", "-j", "link")
	if err != nil {
		return f, errors.New("nic offload collector: " + err.Error())
	}
	var links []ipLinkEntry
	if err := json.Unmarshal(out, &links); err != nil {
		return f, errors.New("nic offload collector: parsing ip link: " + err.Error())
	}

	perNIC := map[string]nicOffloadFacts{}
	sawPHC := false

	for _, link := range links {
		if link.LinkType != "ether" {
			continue
		}
		entry := nicOffloadFacts{PHCIndex: -1}

		if out, err := c.Runner.Run(ctx, "ethtool", "-i", link.IfName); err == nil {
			entry.Driver = parseEthtoolDriverInfo(out)
		}
		if out, err := c.Runner.Run(ctx, "ethtool", "-g", link.IfName); err == nil {
			entry.RingsMax, entry.RingsCurrent = parseEthtoolMaxCurrent(out)
		}
		if out, err := c.Runner.Run(ctx, "ethtool", "-l", link.IfName); err == nil {
			entry.ChannelsMax, entry.ChannelsCurrent = parseEthtoolMaxCurrent(out)
		}
		if out, err := c.Runner.Run(ctx, "ethtool", "-T", link.IfName); err == nil {
			entry.TimestampCaps, entry.PHCIndex = parseEthtoolTimestamping(out)
		}
		if entry.PHCIndex >= 0 {
			sawPHC = true
		}
		perNIC[link.IfName] = entry
	}

	if len(perNIC) == 0 {
		return f, nil
	}
	f.Raw["nic_offload"] = perNIC
	if sawPHC {
		f.Capabilities = []string{"ptp_hardware_clock"}
	}

	return f, nil
}

// parseEthtoolDriverInfo parses `ethtool -i` key: value lines, e.g.
//
//	driver: mlx5_core
//	version: 6.8.0-41-generic
//	firmware-version: 22.36.1010 (MT_0000000437)
//	bus-info: 0000:41:00.0
func parseEthtoolDriverInfo(out []byte) map[string]string {
	info := map[string]string{}
	for _, line := range strings.Split(string(out), "\n") {
		key, value, ok := strings.Cut(line, ":")
		if !ok {
			continue
		}
		key = strings.TrimSpace(key)
		value = strings.TrimSpace(value)
		switch key {
		case "driver", "version", "firmware-version", "bus-info":
			if value != "" {
				info[key] = value
			}
		}
	}
	return info
}

// parseEthtoolMaxCurrent parses the two-section output shared by
// `ethtool -g` and `ethtool -l`:
//
//	Ring parameters for ens1f0:
//	Pre-set maximums:
//	RX:		8192
//	TX:		8192
//	Current hardware settings:
//	RX:		1024
//	TX:		1024
//
// "n/a" values are omitted rather than recorded as zero, since zero is a
// legal configured value for some fields.
func parseEthtoolMaxCurrent(out []byte) (max, current map[string]int) {
	max = map[string]int{}
	current = map[string]int{}
	target := max

	for _, line := range strings.Split(string(out), "\n") {
		line = strings.TrimSpace(line)
		switch {
		case line == "":
			continue
		case strings.HasPrefix(line, "Pre-set maximums"):
			target = max
			continue
		case strings.HasPrefix(line, "Current hardware settings"):
			target = current
			continue
		case strings.HasSuffix(line, ":"):
			// The "Ring parameters for X:" header line.
			continue
		}
		key, value, ok := strings.Cut(line, ":")
		if !ok {
			continue
		}
		value = strings.TrimSpace(value)
		if value == "n/a" || value == "" {
			continue
		}
		target[strings.ToLower(strings.TrimSpace(key))] = atoiOrZero(value)
	}

	if len(max) == 0 {
		max = nil
	}
	if len(current) == 0 {
		current = nil
	}
	return max, current
}

// parseEthtoolTimestamping parses `ethtool -T`, returning the capability
// names listed under "Capabilities:" and the PTP hardware clock index
// ("PTP Hardware Clock: 0"), or -1 when the device has none ("none").
func parseEthtoolTimestamping(out []byte) (caps []string, phcIndex int) {
	phcIndex = -1
	inCaps := false
	for _, rawLine := range strings.Split(string(out), "\n") {
		line := strings.TrimSpace(rawLine)
		if line == "" {
			continue
		}
		if strings.HasPrefix(line, "Capabilities:") {
			inCaps = true
			continue
		}
		if strings.HasPrefix(line, "PTP Hardware Clock:") {
			inCaps = false
			_, value, _ := strings.Cut(line, ":")
			value = strings.TrimSpace(value)
			if value != "none" {
				if n := atoiOrZero(value); n > 0 || value == "0" {
					phcIndex = n
				}
			}
			continue
		}
		if strings.HasSuffix(line, ":") {
			// Any other section header ends the capabilities list.
			inCaps = false
			continue
		}
		if inCaps {
			caps = append(caps, line)
		}
	}
	return caps, phcIndex
}
