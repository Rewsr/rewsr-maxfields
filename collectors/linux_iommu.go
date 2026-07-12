package collectors

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/Rewsr/rewsr-facts/facts"
)

// IOMMUCollector reports whether the IOMMU is actually enabled and
// populated on this host. Device passthrough into TEE guests and safe
// SR-IOV both stand on the IOMMU, and the difference between "the CPU has
// one" and "the kernel is using one" is exactly the kind of fact that gets
// assumed instead of collected. The populated group directory is the
// ground truth: groups only appear when the kernel actually attached an
// IOMMU driver.
type IOMMUCollector struct {
	// SysRoot is prepended to every path this collector reads. The zero
	// value means the real filesystem root; tests point it at a fixture
	// tree instead.
	SysRoot string
}

// NewIOMMUCollector returns an IOMMUCollector reading the real root.
func NewIOMMUCollector() *IOMMUCollector {
	return &IOMMUCollector{}
}

func (c *IOMMUCollector) root() string {
	if c.SysRoot == "" {
		return "/"
	}
	return c.SysRoot
}

// iommuCmdlineFlags are the kernel command line parameters that shape
// IOMMU behavior, carried verbatim into Raw so an operator can see why a
// host is in the state it is.
var iommuCmdlinePrefixes = []string{
	"intel_iommu=",
	"amd_iommu=",
	"iommu=",
	"iommu.passthrough=",
	"iommu.strict=",
}

// Collect reads IOMMU state. Errors only when nothing at all was readable,
// which is the non-Linux case; a Linux host with the IOMMU off reports
// zero groups and no capability, which is the honest answer.
func (c *IOMMUCollector) Collect(ctx context.Context, serverID string) (*facts.ServerFacts, error) {
	f := &facts.ServerFacts{
		ServerID:   serverID,
		Source:     "linux_iommu",
		ObservedAt: time.Now().UTC(),
		Raw:        map[string]any{},
	}

	sawAnySource := false
	raw := map[string]any{}

	groups := 0
	if entries, err := os.ReadDir(filepath.Join(c.root(), "sys/kernel/iommu_groups")); err == nil {
		sawAnySource = true
		for _, e := range entries {
			if e.IsDir() {
				groups++
			}
		}
	}
	raw["groups"] = groups

	if entries, err := os.ReadDir(filepath.Join(c.root(), "sys/class/iommu")); err == nil {
		sawAnySource = true
		var units []string
		for _, e := range entries {
			units = append(units, e.Name())
		}
		if len(units) > 0 {
			raw["units"] = units
		}
	}

	var flags []string
	passthrough := false
	if data, err := os.ReadFile(filepath.Join(c.root(), "proc/cmdline")); err == nil {
		sawAnySource = true
		for _, tok := range strings.Fields(string(data)) {
			for _, prefix := range iommuCmdlinePrefixes {
				if strings.HasPrefix(tok, prefix) {
					flags = append(flags, tok)
				}
			}
			if tok == "iommu=pt" || tok == "iommu.passthrough=1" {
				passthrough = true
			}
		}
	}
	if len(flags) > 0 {
		raw["cmdline_flags"] = flags
	}

	if !sawAnySource {
		return f, errors.New("iommu collector: no IOMMU-related sources readable under " + c.root())
	}

	f.Raw["iommu"] = raw

	var caps []string
	if groups > 0 {
		caps = append(caps, "iommu")
		if passthrough {
			caps = append(caps, "iommu_passthrough")
		}
	}
	f.Capabilities = caps

	return f, nil
}
