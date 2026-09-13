package collectors

import (
	"context"
	"encoding/json"
	"errors"
	"os/exec"
	"time"

	"github.com/Rewsr/rewsr-maxfields/facts"
	"github.com/Rewsr/rewsr-maxfields/runner"
)

// LLDPCollector reads switch neighbor announcements via lldpd's lldpctl.
// Knowing which switch and port each NIC lands on is what turns a pile of
// servers into a topology: it gives failure-domain hints for free and
// catches miscabled hosts before they show up as mystery latency.
type LLDPCollector struct {
	Runner runner.CommandRunner

	// LookPath resolves a command name to a path, or errors if the
	// command isn't installed. Defaults to exec.LookPath; overridable in
	// tests.
	LookPath func(file string) (string, error)
}

// NewLLDPCollector returns an LLDPCollector running commands through r.
func NewLLDPCollector(r runner.CommandRunner) *LLDPCollector {
	return &LLDPCollector{Runner: r, LookPath: exec.LookPath}
}

// lldpNeighbor is one neighbor announcement as carried in
// Raw["lldp_neighbors"].
type lldpNeighbor struct {
	LocalPort    string `json:"local_port"`
	ChassisName  string `json:"chassis_name,omitempty"`
	ChassisDescr string `json:"chassis_descr,omitempty"`
	MgmtIP       string `json:"mgmt_ip,omitempty"`
	PortID       string `json:"port_id,omitempty"`
	PortDescr    string `json:"port_descr,omitempty"`
}

// Collect gathers LLDP neighbors. Zero neighbors (lldpd running but the
// switch is quiet, or no lldpd peers) is a valid answer, not an error.
// When every neighbor agrees on one chassis, that chassis becomes a
// failure-domain hint; the merge logic only uses it if no earlier
// collector (provisioning, typically) already set one.
func (c *LLDPCollector) Collect(ctx context.Context, serverID string) (*facts.ServerFacts, error) {
	f := &facts.ServerFacts{
		ServerID:   serverID,
		Source:     "linux_lldp",
		ObservedAt: time.Now().UTC(),
		Raw:        map[string]any{},
	}

	if c.Runner == nil {
		return f, errors.New("lldp collector: no CommandRunner configured")
	}
	lookPath := c.LookPath
	if lookPath == nil {
		lookPath = exec.LookPath
	}
	if err := requireCommand(lookPath, "lldpctl"); err != nil {
		return f, errors.New("lldp collector: " + err.Error())
	}

	out, err := c.Runner.Run(ctx, "lldpctl", "-f", "json")
	if err != nil {
		return f, errors.New("lldp collector: " + err.Error())
	}

	neighbors, err := parseLldpctlJSON(out)
	if err != nil {
		return f, errors.New("lldp collector: " + err.Error())
	}
	if len(neighbors) == 0 {
		return f, nil
	}

	f.Raw["lldp_neighbors"] = neighbors

	// A single distinct chassis across every port means the host hangs
	// off one switch, which is the sharpest failure-domain fact LLDP can
	// give. Multiple chassis (dual-homed) means no single domain, so no
	// hint is set.
	chassis := ""
	single := true
	for _, n := range neighbors {
		if n.ChassisName == "" {
			continue
		}
		if chassis == "" {
			chassis = n.ChassisName
		} else if chassis != n.ChassisName {
			single = false
			break
		}
	}
	if single && chassis != "" {
		f.FailureDomain = "switch:" + chassis
	}

	return f, nil
}

// parseLldpctlJSON parses `lldpctl -f json`. The structure is
// {"lldp": {"interface": ...}} where interface is a single object for one
// port or an array of single-key objects for several, and chassis is keyed
// by the neighbor's system name:
//
//	{"lldp": {"interface": [{"ens1f0np0": {
//	    "chassis": {"leaf01": {"descr": "...", "mgmt-ip": "10.0.0.11"}},
//	    "port": {"id": {"type": "ifname", "value": "swp1"}, "descr": "to-server"}
//	}}]}}
//
// lldpd's JSON collapses single-element containers, so every layer here
// tolerates both the object and the array form.
func parseLldpctlJSON(out []byte) ([]lldpNeighbor, error) {
	var doc map[string]any
	if err := json.Unmarshal(out, &doc); err != nil {
		return nil, err
	}

	lldp, _ := doc["lldp"].(map[string]any)
	if lldp == nil {
		return nil, nil
	}

	var neighbors []lldpNeighbor
	for _, ifaceObj := range asObjectList(lldp["interface"]) {
		for localPort, v := range ifaceObj {
			entry, _ := v.(map[string]any)
			if entry == nil {
				continue
			}
			n := lldpNeighbor{LocalPort: localPort}

			if chassisWrap, ok := entry["chassis"].(map[string]any); ok {
				for name, cv := range chassisWrap {
					n.ChassisName = name
					if cm, ok := cv.(map[string]any); ok {
						n.ChassisDescr, _ = cm["descr"].(string)
						switch ip := cm["mgmt-ip"].(type) {
						case string:
							n.MgmtIP = ip
						case []any:
							if len(ip) > 0 {
								n.MgmtIP, _ = ip[0].(string)
							}
						}
					}
					break
				}
			}

			if port, ok := entry["port"].(map[string]any); ok {
				n.PortDescr, _ = port["descr"].(string)
				if id, ok := port["id"].(map[string]any); ok {
					n.PortID, _ = id["value"].(string)
				}
			}

			neighbors = append(neighbors, n)
		}
	}
	return neighbors, nil
}

// asObjectList normalizes lldpd's collapsed containers: a JSON object
// becomes a one-element list, a JSON array yields its object elements.
func asObjectList(v any) []map[string]any {
	switch t := v.(type) {
	case map[string]any:
		return []map[string]any{t}
	case []any:
		var out []map[string]any
		for _, e := range t {
			if m, ok := e.(map[string]any); ok {
				out = append(out, m)
			}
		}
		return out
	}
	return nil
}
