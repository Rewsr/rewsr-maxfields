package collectors

import (
	"context"
	"encoding/json"
	"strconv"
	"strings"

	"github.com/Rewsr/rewsr-facts/facts"
)

// lsblkJSON mirrors `lsblk -J -b -o NAME,TYPE,SIZE,ROTA` output.
type lsblkJSON struct {
	BlockDevices []lsblkDevice `json:"blockdevices"`
}

type lsblkDevice struct {
	Name string   `json:"name"`
	Type string   `json:"type"`
	Size flexInt  `json:"size"`
	Rota flexBool `json:"rota"`
}

// flexInt unmarshals a JSON number or a JSON string containing a number.
// Different util-linux versions emit lsblk's numeric fields differently
// (raw number vs quoted string), so we accept either.
type flexInt int64

func (n *flexInt) UnmarshalJSON(b []byte) error {
	s := strings.Trim(string(b), `"`)
	if s == "" || s == "null" {
		*n = 0
		return nil
	}
	v, err := strconv.ParseInt(s, 10, 64)
	if err != nil {
		return err
	}
	*n = flexInt(v)
	return nil
}

// flexBool unmarshals a JSON bool, or a JSON string/number of "0"/"1",
// matching the same lsblk version skew as flexInt.
type flexBool bool

func (b *flexBool) UnmarshalJSON(raw []byte) error {
	s := strings.Trim(strings.TrimSpace(string(raw)), `"`)
	switch s {
	case "true", "1":
		*b = true
	case "false", "0", "", "null":
		*b = false
	default:
		var v bool
		if err := json.Unmarshal(raw, &v); err != nil {
			return err
		}
		*b = flexBool(v)
	}
	return nil
}

func (c *LinuxHostCollector) collectDisks(ctx context.Context, lookPath func(string) (string, error), f *facts.ServerFacts) error {
	if err := requireCommand(lookPath, "lsblk"); err != nil {
		return err
	}

	out, err := c.Runner.Run(ctx, "lsblk", "-J", "-b", "-o", "NAME,TYPE,SIZE,ROTA")
	if err != nil {
		return err
	}

	var parsed lsblkJSON
	if err := json.Unmarshal(out, &parsed); err != nil {
		return err
	}

	var disks []facts.DiskFacts
	for _, dev := range parsed.BlockDevices {
		// Only whole disks, not partitions, loop devices, or optical
		// drives; those aren't useful bare-metal capability signals.
		if dev.Type != "disk" {
			continue
		}
		disks = append(disks, facts.DiskFacts{
			Name:       dev.Name,
			Type:       diskType(dev.Name, bool(dev.Rota)),
			SizeGB:     int(int64(dev.Size) / (1024 * 1024 * 1024)),
			Rotational: bool(dev.Rota),
		})
	}
	f.Disks = disks

	return nil
}

// diskType infers nvme/ssd/hdd. lsblk's TYPE column only says "disk" vs
// "part" vs "rom"; it does not distinguish media type, so we use the
// device name (nvme* devices are always NVMe) and ROTA (0 for any
// non-rotational media, i.e. SSD, since NVMe already got caught above).
func diskType(name string, rotational bool) string {
	if strings.HasPrefix(name, "nvme") {
		return "nvme"
	}
	if rotational {
		return "hdd"
	}
	return "ssd"
}
