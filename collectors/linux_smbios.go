package collectors

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/Rewsr/rewsr-facts/facts"
)

// SMBIOSCollector reads the DMI identity of the server from
// /sys/class/dmi/id: vendor, product, serial, board, BIOS version and date,
// and chassis type. This is how the fleet distinguishes a Dell R7525 from a
// Supermicro 1U without trusting an external inventory system, and it needs
// no root except for the serial and UUID files, which the kernel restricts
// to root. Fields we cannot read are simply omitted rather than failing the
// whole collector.
type SMBIOSCollector struct {
	// SysRoot is prepended to every path this collector reads. The zero
	// value means the real filesystem root; tests point it at a fixture
	// tree instead.
	SysRoot string
}

// NewSMBIOSCollector returns an SMBIOSCollector reading from the real root.
func NewSMBIOSCollector() *SMBIOSCollector {
	return &SMBIOSCollector{}
}

func (c *SMBIOSCollector) root() string {
	if c.SysRoot == "" {
		return "/"
	}
	return c.SysRoot
}

// dmiFields are the /sys/class/dmi/id files worth carrying as facts. The
// key is the file name and also the key used inside Raw["dmi"].
var dmiFields = []string{
	"sys_vendor",
	"product_name",
	"product_serial",
	"product_uuid",
	"board_vendor",
	"board_name",
	"bios_vendor",
	"bios_version",
	"bios_date",
	"chassis_type",
}

// Collect reads the DMI id directory. It errors only when the directory
// itself is unreadable (non-Linux hosts, or a kernel without DMI), and
// otherwise returns whatever subset of fields was readable.
func (c *SMBIOSCollector) Collect(ctx context.Context, serverID string) (*facts.ServerFacts, error) {
	f := &facts.ServerFacts{
		ServerID:   serverID,
		Source:     "linux_smbios",
		ObservedAt: time.Now().UTC(),
		Raw:        map[string]any{},
	}

	dir := filepath.Join(c.root(), "sys/class/dmi/id")
	if _, err := os.Stat(dir); err != nil {
		return f, errors.New("smbios collector: " + dir + " not readable: " + err.Error())
	}

	dmi := map[string]any{}
	for _, field := range dmiFields {
		data, err := os.ReadFile(filepath.Join(dir, field))
		if err != nil {
			// Serial and UUID are root-only (mode 0400); everything else
			// missing usually means the platform just does not populate
			// that record. Either way the right move is to skip the field.
			continue
		}
		value := strings.TrimSpace(string(data))
		if value == "" {
			continue
		}
		dmi[field] = value
	}

	if ct, ok := dmi["chassis_type"].(string); ok {
		if n, err := strconv.Atoi(ct); err == nil {
			if name, known := chassisTypeNames[n]; known {
				dmi["chassis_type_name"] = name
			}
		}
	}

	if len(dmi) == 0 {
		return f, errors.New("smbios collector: no readable fields under " + dir)
	}
	f.Raw["dmi"] = dmi

	return f, nil
}

// chassisTypeNames maps the SMBIOS 3.x System Enclosure type byte to its
// specified name. The numbers come from the SMBIOS specification table for
// System Enclosure or Chassis Types; server fleets mostly see 17, 23, and
// the blade pair 28/29.
var chassisTypeNames = map[int]string{
	1:  "Other",
	2:  "Unknown",
	3:  "Desktop",
	4:  "Low Profile Desktop",
	5:  "Pizza Box",
	6:  "Mini Tower",
	7:  "Tower",
	8:  "Portable",
	9:  "Laptop",
	10: "Notebook",
	11: "Hand Held",
	12: "Docking Station",
	13: "All In One",
	14: "Sub Notebook",
	15: "Space-saving",
	16: "Lunch Box",
	17: "Main Server Chassis",
	18: "Expansion Chassis",
	19: "Sub Chassis",
	20: "Bus Expansion Chassis",
	21: "Peripheral Chassis",
	22: "RAID Chassis",
	23: "Rack Mount Chassis",
	24: "Sealed-case PC",
	25: "Multi-system Chassis",
	26: "Compact PCI",
	27: "Advanced TCA",
	28: "Blade",
	29: "Blade Enclosure",
	30: "Tablet",
	31: "Convertible",
	32: "Detachable",
	33: "IoT Gateway",
	34: "Embedded PC",
	35: "Mini PC",
	36: "Stick PC",
}
