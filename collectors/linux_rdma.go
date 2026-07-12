package collectors

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/Rewsr/rewsr-facts/facts"
	"github.com/Rewsr/rewsr-facts/runner"
)

// RDMACollector inventories RDMA devices: ConnectX-style NICs doing RoCE,
// real InfiniBand HCAs, and anything else registered with the kernel RDMA
// subsystem. For a fabric between TEEs this is the single most important
// hardware fact after the TEE itself, so it gets its own collector instead
// of hanging off the generic NIC scan.
//
// The primary source is `rdma link show -j` from iproute2, which is JSON
// and stable. When the rdma tool is missing we fall back to parsing
// `ibv_devinfo` from rdma-core. Link layer (InfiniBand vs Ethernet, which
// is what distinguishes IB from RoCE) and firmware come from
// /sys/class/infiniband when readable.
type RDMACollector struct {
	Runner runner.CommandRunner

	// LookPath resolves a command name to a path, or errors if the
	// command isn't installed. Defaults to exec.LookPath; overridable in
	// tests.
	LookPath func(file string) (string, error)

	// SysRoot is prepended to every sysfs path this collector reads. The
	// zero value means the real filesystem root; tests point it at a
	// fixture tree instead.
	SysRoot string
}

// NewRDMACollector returns an RDMACollector running commands through r.
func NewRDMACollector(r runner.CommandRunner) *RDMACollector {
	return &RDMACollector{Runner: r, LookPath: exec.LookPath}
}

func (c *RDMACollector) root() string {
	if c.SysRoot == "" {
		return "/"
	}
	return c.SysRoot
}

// rdmaDevice is one RDMA device port as carried in Raw["rdma"].
type rdmaDevice struct {
	Device        string `json:"device"`
	Port          int    `json:"port"`
	State         string `json:"state"`
	PhysicalState string `json:"physical_state,omitempty"`
	Netdev        string `json:"netdev,omitempty"`
	LinkLayer     string `json:"link_layer,omitempty"`
	Transport     string `json:"transport,omitempty"`
	FirmwareVer   string `json:"firmware_version,omitempty"`
}

// Collect gathers RDMA facts. A host whose RDMA subsystem is present but
// empty (rdma tool works, zero links) is a valid answer, not an error:
// the fact is "no RDMA here".
func (c *RDMACollector) Collect(ctx context.Context, serverID string) (*facts.ServerFacts, error) {
	f := &facts.ServerFacts{
		ServerID:   serverID,
		Source:     "linux_rdma",
		ObservedAt: time.Now().UTC(),
		Raw:        map[string]any{},
	}

	if c.Runner == nil {
		return f, errors.New("rdma collector: no CommandRunner configured")
	}
	lookPath := c.LookPath
	if lookPath == nil {
		lookPath = exec.LookPath
	}

	devices, err := c.gather(ctx, lookPath)
	if err != nil {
		return f, errors.New("rdma collector: " + err.Error())
	}

	for i := range devices {
		c.enrichFromSysfs(&devices[i])
	}

	if len(devices) == 0 {
		return f, nil
	}

	f.Raw["rdma"] = devices
	f.NetworkModes = []string{"rdma"}

	caps := []string{"rdma"}
	sawIB, sawRoCE, sawActive := false, false, false
	for _, d := range devices {
		switch d.LinkLayer {
		case "InfiniBand":
			sawIB = true
		case "Ethernet":
			sawRoCE = true
		}
		if strings.EqualFold(d.State, "ACTIVE") {
			sawActive = true
		}
	}
	if sawIB {
		caps = append(caps, "infiniband")
	}
	if sawRoCE {
		caps = append(caps, "roce")
	}
	if sawActive {
		caps = append(caps, "rdma_active")
	}
	f.Capabilities = caps

	return f, nil
}

// gather picks the best available source: rdma link JSON first, then
// ibv_devinfo. Only when neither tool exists do we report an error.
func (c *RDMACollector) gather(ctx context.Context, lookPath func(string) (string, error)) ([]rdmaDevice, error) {
	rdmaErr := requireCommand(lookPath, "rdma")
	if rdmaErr == nil {
		out, err := c.Runner.Run(ctx, "rdma", "link", "show", "-j")
		if err != nil {
			return nil, err
		}
		return parseRdmaLinkJSON(out)
	}

	ibvErr := requireCommand(lookPath, "ibv_devinfo")
	if ibvErr == nil {
		out, err := c.Runner.Run(ctx, "ibv_devinfo")
		if err != nil {
			return nil, err
		}
		return parseIbvDevinfo(out), nil
	}

	return nil, errors.New(rdmaErr.Error() + "; " + ibvErr.Error())
}

// rdmaLinkEntry mirrors one element of `rdma link show -j` output.
type rdmaLinkEntry struct {
	IfIndex       int    `json:"ifindex"`
	IfName        string `json:"ifname"`
	Port          int    `json:"port"`
	State         string `json:"state"`
	PhysicalState string `json:"physical_state"`
	Netdev        string `json:"netdev"`
}

func parseRdmaLinkJSON(out []byte) ([]rdmaDevice, error) {
	var entries []rdmaLinkEntry
	if err := json.Unmarshal(out, &entries); err != nil {
		return nil, err
	}
	devices := make([]rdmaDevice, 0, len(entries))
	for _, e := range entries {
		devices = append(devices, rdmaDevice{
			Device:        e.IfName,
			Port:          e.Port,
			State:         e.State,
			PhysicalState: e.PhysicalState,
			Netdev:        e.Netdev,
		})
	}
	return devices, nil
}

// parseIbvDevinfo parses the human-oriented ibv_devinfo output. The format
// is stable enough to rely on: an hca_id line opens each device stanza,
// "port:" lines open per-port blocks, and every other line is a tab-padded
// "key:value" pair, e.g.
//
//	hca_id:	mlx5_0
//		transport:			InfiniBand (0)
//		fw_ver:				20.39.2048
//			port:	1
//				state:			PORT_ACTIVE (4)
//				link_layer:		Ethernet
func parseIbvDevinfo(out []byte) []rdmaDevice {
	var devices []rdmaDevice
	var hca, transport, fwVer string
	var cur *rdmaDevice

	flush := func() {
		if cur != nil {
			devices = append(devices, *cur)
			cur = nil
		}
	}

	for _, line := range strings.Split(string(out), "\n") {
		key, value, ok := strings.Cut(line, ":")
		if !ok {
			continue
		}
		key = strings.TrimSpace(key)
		value = strings.TrimSpace(value)

		switch key {
		case "hca_id":
			flush()
			hca, transport, fwVer = value, "", ""
		case "transport":
			transport = trimParenthetical(value)
		case "fw_ver":
			fwVer = value
		case "port":
			flush()
			port, _ := strconv.Atoi(value)
			cur = &rdmaDevice{Device: hca, Port: port, Transport: transport, FirmwareVer: fwVer}
		case "state":
			if cur != nil {
				cur.State = normalizeIbvPortState(trimParenthetical(value))
			}
		case "link_layer":
			if cur != nil {
				cur.LinkLayer = value
			}
		}
	}
	flush()
	return devices
}

// trimParenthetical turns "InfiniBand (0)" into "InfiniBand".
func trimParenthetical(v string) string {
	if idx := strings.Index(v, "("); idx >= 0 {
		return strings.TrimSpace(v[:idx])
	}
	return v
}

// normalizeIbvPortState maps ibv_devinfo's PORT_ACTIVE style names onto the
// vocabulary `rdma link` uses, so downstream consumers see one state
// vocabulary regardless of which tool was available.
func normalizeIbvPortState(s string) string {
	return strings.ToUpper(strings.TrimPrefix(s, "PORT_"))
}

// enrichFromSysfs fills link layer and firmware from
// /sys/class/infiniband/<device>, which is readable without privileges and
// present whenever the RDMA subsystem knows the device.
func (c *RDMACollector) enrichFromSysfs(d *rdmaDevice) {
	base := filepath.Join(c.root(), "sys/class/infiniband", d.Device)
	if d.LinkLayer == "" {
		p := filepath.Join(base, "ports", strconv.Itoa(d.Port), "link_layer")
		if data, err := os.ReadFile(p); err == nil {
			d.LinkLayer = strings.TrimSpace(string(data))
		}
	}
	if d.FirmwareVer == "" {
		if data, err := os.ReadFile(filepath.Join(base, "fw_ver")); err == nil {
			d.FirmwareVer = strings.TrimSpace(string(data))
		}
	}
}
