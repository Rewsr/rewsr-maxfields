package collectors

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"

	"github.com/Rewsr/rewsr-maxfields/facts"
	"github.com/Rewsr/rewsr-maxfields/runner"
)

// NetTopologyCollector records how the host's interfaces are composed:
// bonds and their LACP state, VLAN subinterfaces, and bridges. A fabric
// endpoint behind an active 802.3ad bond has different failure and
// throughput behavior than one on a single link, and that difference
// belongs in the facts store, not in tribal knowledge.
//
// Interface kinds come from `ip -j -d link`; per-bond detail comes from
// /proc/net/bonding/<name>, which the bonding driver keeps current.
type NetTopologyCollector struct {
	Runner runner.CommandRunner

	// LookPath resolves a command name to a path, or errors if the
	// command isn't installed. Defaults to exec.LookPath; overridable in
	// tests.
	LookPath func(file string) (string, error)

	// SysRoot is prepended to the /proc paths this collector reads. The
	// zero value means the real filesystem root; tests point it at a
	// fixture tree instead.
	SysRoot string
}

// NewNetTopologyCollector returns a NetTopologyCollector running commands
// through r.
func NewNetTopologyCollector(r runner.CommandRunner) *NetTopologyCollector {
	return &NetTopologyCollector{Runner: r, LookPath: exec.LookPath}
}

func (c *NetTopologyCollector) root() string {
	if c.SysRoot == "" {
		return "/"
	}
	return c.SysRoot
}

// ipLinkDetail mirrors the fields of `ip -j -d link` this collector needs.
type ipLinkDetail struct {
	IfName   string `json:"ifname"`
	LinkInfo struct {
		InfoKind string `json:"info_kind"`
	} `json:"linkinfo"`
}

// bondFacts is one bond as carried in Raw["net_topology"].
type bondFacts struct {
	Name       string      `json:"name"`
	Mode       string      `json:"mode"`
	HashPolicy string      `json:"hash_policy,omitempty"`
	MIIStatus  string      `json:"mii_status,omitempty"`
	LACPActive string      `json:"lacp_active,omitempty"`
	LACPRate   string      `json:"lacp_rate,omitempty"`
	Slaves     []bondSlave `json:"slaves,omitempty"`
}

type bondSlave struct {
	Name      string `json:"name"`
	MIIStatus string `json:"mii_status,omitempty"`
	SpeedMbps int    `json:"speed_mbps,omitempty"`
}

// Collect gathers interface composition facts. A host with no bonds,
// VLANs, or bridges returns empty facts and no error; that is the common
// answer and it is not a failure.
func (c *NetTopologyCollector) Collect(ctx context.Context, serverID string) (*facts.ServerFacts, error) {
	f := &facts.ServerFacts{
		ServerID:   serverID,
		Source:     "linux_net_topology",
		ObservedAt: time.Now().UTC(),
		Raw:        map[string]any{},
	}

	if c.Runner == nil {
		return f, errors.New("net topology collector: no CommandRunner configured")
	}
	lookPath := c.LookPath
	if lookPath == nil {
		lookPath = exec.LookPath
	}
	if err := requireCommand(lookPath, "ip"); err != nil {
		return f, errors.New("net topology collector: " + err.Error())
	}

	out, err := c.Runner.Run(ctx, "ip", "-j", "-d", "link")
	if err != nil {
		return f, errors.New("net topology collector: " + err.Error())
	}
	var links []ipLinkDetail
	if err := json.Unmarshal(out, &links); err != nil {
		return f, errors.New("net topology collector: parsing ip link: " + err.Error())
	}

	var bonds []bondFacts
	var vlans, bridges []string
	for _, link := range links {
		switch link.LinkInfo.InfoKind {
		case "bond":
			b := bondFacts{Name: link.IfName}
			path := filepath.Join(c.root(), "proc/net/bonding", link.IfName)
			if data, err := os.ReadFile(path); err == nil {
				b = parseBondFile(link.IfName, data)
			}
			bonds = append(bonds, b)
		case "vlan":
			vlans = append(vlans, link.IfName)
		case "bridge":
			bridges = append(bridges, link.IfName)
		}
	}

	if len(bonds) == 0 && len(vlans) == 0 && len(bridges) == 0 {
		return f, nil
	}

	topo := map[string]any{}
	var modes []string
	if len(bonds) > 0 {
		topo["bonds"] = bonds
		modes = append(modes, "bonded")
		for _, b := range bonds {
			if strings.Contains(b.Mode, "802.3ad") {
				modes = append(modes, "lacp")
				break
			}
		}
	}
	if len(vlans) > 0 {
		topo["vlans"] = vlans
		modes = append(modes, "vlan_tagged")
	}
	if len(bridges) > 0 {
		topo["bridges"] = bridges
		modes = append(modes, "bridged")
	}

	f.Raw["net_topology"] = topo
	f.NetworkModes = modes

	return f, nil
}

// parseBondFile parses /proc/net/bonding/<name>. The file is a flat list
// of "Key: value" lines where everything before the first "Slave
// Interface:" line describes the bond and each "Slave Interface:" line
// opens one slave block, e.g.
//
//	Bonding Mode: IEEE 802.3ad Dynamic link aggregation
//	Transmit Hash Policy: layer3+4 (1)
//	MII Status: up
//	LACP active: on
//	LACP rate: fast
//
//	Slave Interface: ens1f0np0
//	MII Status: up
//	Speed: 25000 Mbps
func parseBondFile(name string, data []byte) bondFacts {
	b := bondFacts{Name: name}
	var cur *bondSlave

	for _, line := range strings.Split(string(data), "\n") {
		key, value, ok := strings.Cut(line, ":")
		if !ok {
			continue
		}
		key = strings.TrimSpace(key)
		value = strings.TrimSpace(value)

		if key == "Slave Interface" {
			b.Slaves = append(b.Slaves, bondSlave{Name: value})
			cur = &b.Slaves[len(b.Slaves)-1]
			continue
		}

		if cur != nil {
			switch key {
			case "MII Status":
				cur.MIIStatus = value
			case "Speed":
				cur.SpeedMbps = atoiOrZero(strings.TrimSuffix(value, " Mbps"))
			}
			continue
		}

		switch key {
		case "Bonding Mode":
			b.Mode = value
		case "Transmit Hash Policy":
			b.HashPolicy = value
		case "MII Status":
			b.MIIStatus = value
		case "LACP active":
			b.LACPActive = value
		case "LACP rate":
			b.LACPRate = value
		}
	}
	return b
}
