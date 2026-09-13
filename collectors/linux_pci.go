package collectors

import (
	"context"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/Rewsr/rewsr-maxfields/facts"
	"github.com/Rewsr/rewsr-maxfields/runner"
)

// PCICollector inventories the PCIe devices that matter for placing fabric
// workloads: NICs, NVMe controllers, and accelerators, along with each
// endpoint's negotiated and maximum link speed and width from sysfs. Link
// generation is a scheduling fact in its own right: a ConnectX-7 stuck in
// a Gen3 slot is not the NIC the inventory system thinks it is.
type PCICollector struct {
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

// NewPCICollector returns a PCICollector running commands through r.
func NewPCICollector(r runner.CommandRunner) *PCICollector {
	return &PCICollector{Runner: r, LookPath: exec.LookPath}
}

func (c *PCICollector) root() string {
	if c.SysRoot == "" {
		return "/"
	}
	return c.SysRoot
}

// pciDevice is one PCIe function as carried in Raw["pci"].
type pciDevice struct {
	Address     string  `json:"address"`
	ClassName   string  `json:"class_name"`
	ClassID     string  `json:"class_id"`
	VendorName  string  `json:"vendor_name"`
	VendorID    string  `json:"vendor_id"`
	DeviceName  string  `json:"device_name"`
	DeviceID    string  `json:"device_id"`
	Revision    string  `json:"revision,omitempty"`
	CurLinkGTs  float64 `json:"current_link_gts,omitempty"`
	MaxLinkGTs  float64 `json:"max_link_gts,omitempty"`
	CurLinkX    int     `json:"current_link_width,omitempty"`
	MaxLinkX    int     `json:"max_link_width,omitempty"`
	SRIOVTotal  int     `json:"sriov_total_vfs,omitempty"`
	SRIOVActive int     `json:"sriov_num_vfs,omitempty"`
}

// interestingPCIClasses filters the inventory down to device classes a
// fabric scheduler cares about. Bridges, USB controllers, and the rest of
// the platform plumbing are noise at this layer, and a dual-socket server
// carries dozens of them.
var interestingPCIClasses = map[string]string{
	"0100": "scsi",
	"0104": "raid",
	"0106": "sata",
	"0107": "sas",
	"0108": "nvme",
	"0200": "ethernet",
	"0207": "infiniband",
	"0280": "network_other",
	"0300": "display",
	"0302": "gpu_3d",
	"0b40": "coprocessor",
	"1200": "processing_accelerator",
}

// Collect inventories interesting PCIe functions via `lspci -Dnnmm` and
// enriches each with link facts from sysfs.
func (c *PCICollector) Collect(ctx context.Context, serverID string) (*facts.ServerFacts, error) {
	f := &facts.ServerFacts{
		ServerID:   serverID,
		Source:     "linux_pci",
		ObservedAt: time.Now().UTC(),
		Raw:        map[string]any{},
	}

	if c.Runner == nil {
		return f, errors.New("pci collector: no CommandRunner configured")
	}
	lookPath := c.LookPath
	if lookPath == nil {
		lookPath = exec.LookPath
	}
	if err := requireCommand(lookPath, "lspci"); err != nil {
		return f, errors.New("pci collector: " + err.Error())
	}

	out, err := c.Runner.Run(ctx, "lspci", "-Dnnmm")
	if err != nil {
		return f, errors.New("pci collector: " + err.Error())
	}

	all := parseLspciMM(out)
	if len(all) == 0 {
		return f, errors.New("pci collector: lspci output parsed to zero devices")
	}

	var kept []pciDevice
	for _, d := range all {
		if _, interesting := interestingPCIClasses[d.ClassID]; !interesting {
			continue
		}
		c.enrichFromSysfs(&d)
		kept = append(kept, d)
	}

	f.Raw["pci"] = map[string]any{
		"total_functions": len(all),
		"devices":         kept,
	}

	var caps []string
	maxGTs := 0.0
	sawAccel, sawSRIOV := false, false
	for _, d := range kept {
		if d.MaxLinkGTs > maxGTs {
			maxGTs = d.MaxLinkGTs
		}
		if d.ClassID == "0302" || d.ClassID == "1200" || d.ClassID == "0b40" {
			sawAccel = true
		}
		if d.SRIOVTotal > 0 {
			sawSRIOV = true
		}
	}
	// Only the best generation present is reported: gen5 implies gen4
	// slots exist, and listing every generation would just pad the
	// capability set.
	switch {
	case maxGTs >= 32:
		caps = append(caps, "pcie_gen5")
	case maxGTs >= 16:
		caps = append(caps, "pcie_gen4")
	}
	if sawAccel {
		caps = append(caps, "gpu_accelerator")
	}
	if sawSRIOV {
		caps = append(caps, "sriov")
	}
	f.Capabilities = caps

	return f, nil
}

// parseLspciMM parses `lspci -Dnnmm` machine-readable output. Each line is
// a domain-qualified slot followed by quoted fields, with unquoted -rXX
// and -pXX tokens for revision and programming interface, e.g.
//
//	0000:c1:00.0 "Ethernet controller [0200]" "Mellanox Technologies [15b3]" "MT2892 Family [ConnectX-6 Dx] [101d]" -r00 "Mellanox Technologies [15b3]" "ConnectX-6 Dx SmartNIC [0083]"
func parseLspciMM(out []byte) []pciDevice {
	var devices []pciDevice
	for _, line := range strings.Split(string(out), "\n") {
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		addr, rest, ok := strings.Cut(line, " ")
		if !ok {
			continue
		}
		fields, tokens := splitLspciFields(rest)
		if len(fields) < 3 {
			continue
		}

		className, classID := splitNameAndID(fields[0])
		vendorName, vendorID := splitNameAndID(fields[1])
		deviceName, deviceID := splitNameAndID(fields[2])

		d := pciDevice{
			Address:    addr,
			ClassName:  className,
			ClassID:    classID,
			VendorName: vendorName,
			VendorID:   vendorID,
			DeviceName: deviceName,
			DeviceID:   deviceID,
		}
		for _, t := range tokens {
			if strings.HasPrefix(t, "-r") {
				d.Revision = strings.TrimPrefix(t, "-r")
			}
		}
		devices = append(devices, d)
	}
	return devices
}

// splitLspciFields separates the quoted fields of one lspci -mm line from
// the unquoted -rXX/-pXX tokens between them.
func splitLspciFields(s string) (fields []string, tokens []string) {
	for i := 0; i < len(s); {
		switch {
		case s[i] == '"':
			end := strings.IndexByte(s[i+1:], '"')
			if end < 0 {
				return fields, tokens
			}
			fields = append(fields, s[i+1:i+1+end])
			i += end + 2
		case s[i] == ' ':
			i++
		default:
			end := strings.IndexByte(s[i:], ' ')
			if end < 0 {
				tokens = append(tokens, s[i:])
				return fields, tokens
			}
			tokens = append(tokens, s[i:i+end])
			i += end
		}
	}
	return fields, tokens
}

// splitNameAndID turns lspci's "Ethernet controller [0200]" form into the
// name and the bracketed id. The id is always the last bracket pair; device
// names like "MT2892 Family [ConnectX-6 Dx]" contain earlier brackets that
// belong to the name.
func splitNameAndID(field string) (name, id string) {
	open := strings.LastIndex(field, "[")
	close := strings.LastIndex(field, "]")
	if open < 0 || close < open {
		return strings.TrimSpace(field), ""
	}
	return strings.TrimSpace(field[:open]), field[open+1 : close]
}

// enrichFromSysfs fills PCIe link speed/width and SR-IOV counts from
// /sys/bus/pci/devices/<address>. Missing files are normal: virtual
// functions have no sriov files, and some platforms hide link attributes
// on integrated endpoints.
func (c *PCICollector) enrichFromSysfs(d *pciDevice) {
	base := filepath.Join(c.root(), "sys/bus/pci/devices", d.Address)

	d.CurLinkGTs = readLinkGTs(filepath.Join(base, "current_link_speed"))
	d.MaxLinkGTs = readLinkGTs(filepath.Join(base, "max_link_speed"))
	d.CurLinkX = readIntFile(filepath.Join(base, "current_link_width"))
	d.MaxLinkX = readIntFile(filepath.Join(base, "max_link_width"))
	d.SRIOVTotal = readIntFile(filepath.Join(base, "sriov_totalvfs"))
	d.SRIOVActive = readIntFile(filepath.Join(base, "sriov_numvfs"))
}

// readLinkGTs parses sysfs link speed files, whose content looks like
// "16.0 GT/s PCIe" on recent kernels and "8 GT/s" on older ones.
func readLinkGTs(path string) float64 {
	data, err := os.ReadFile(path)
	if err != nil {
		return 0
	}
	first, _, _ := strings.Cut(strings.TrimSpace(string(data)), " ")
	v, err := strconv.ParseFloat(first, 64)
	if err != nil {
		return 0
	}
	return v
}

func readIntFile(path string) int {
	data, err := os.ReadFile(path)
	if err != nil {
		return 0
	}
	n, err := strconv.Atoi(strings.TrimSpace(string(data)))
	if err != nil {
		return 0
	}
	return n
}
