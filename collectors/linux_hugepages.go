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

// HugepagesCollector reports the hugepage configuration of the host: the
// default size, the pool counters from /proc/meminfo, and every configured
// size from /sys/kernel/mm/hugepages. AF_XDP UMEMs, DPDK-style data planes,
// and TEE guest memory all want hugepages, and "the operator forgot to
// reserve them" is a classic silent performance cliff, so the reservation
// state is collected as a fact.
type HugepagesCollector struct {
	// SysRoot is prepended to every path this collector reads. The zero
	// value means the real filesystem root; tests point it at a fixture
	// tree instead.
	SysRoot string
}

// NewHugepagesCollector returns a HugepagesCollector reading the real root.
func NewHugepagesCollector() *HugepagesCollector {
	return &HugepagesCollector{}
}

func (c *HugepagesCollector) root() string {
	if c.SysRoot == "" {
		return "/"
	}
	return c.SysRoot
}

// hugepageSize is one configured hugepage size as carried in Raw.
type hugepageSize struct {
	SizeKB int `json:"size_kb"`
	Nr     int `json:"nr"`
	Free   int `json:"free"`
}

// Collect reads hugepage state. Errors only when neither /proc/meminfo nor
// the sysfs hugepages directory is readable.
func (c *HugepagesCollector) Collect(ctx context.Context, serverID string) (*facts.ServerFacts, error) {
	f := &facts.ServerFacts{
		ServerID:   serverID,
		Source:     "linux_hugepages",
		ObservedAt: time.Now().UTC(),
		Raw:        map[string]any{},
	}

	raw := map[string]any{}
	sawAnySource := false

	if data, err := os.ReadFile(filepath.Join(c.root(), "proc/meminfo")); err == nil {
		sawAnySource = true
		mi := parseMeminfoHugepages(data)
		for k, v := range mi {
			raw[k] = v
		}
	}

	sizes := c.readSysfsSizes()
	if sizes != nil {
		sawAnySource = true
		raw["sizes"] = sizes
	}

	if !sawAnySource {
		return f, errors.New("hugepages collector: neither proc/meminfo nor sys/kernel/mm/hugepages readable under " + c.root())
	}

	f.Raw["hugepages"] = raw

	var caps []string
	total, _ := raw["total"].(int)
	if total > 0 {
		caps = append(caps, "hugepages")
	}
	for _, s := range sizes {
		if s.Nr > 0 && total == 0 {
			// Sizes configured via sysfs but meminfo unreadable; still a
			// real reservation.
			caps = append(caps, "hugepages")
		}
		if s.SizeKB == 1048576 && s.Nr > 0 {
			caps = append(caps, "hugepages_1g")
		}
	}
	f.Capabilities = dedupeStrings(caps)

	return f, nil
}

// parseMeminfoHugepages extracts the hugepage fields from /proc/meminfo:
//
//	HugePages_Total:    1024
//	HugePages_Free:      512
//	HugePages_Rsvd:        0
//	HugePages_Surp:        0
//	Hugepagesize:       2048 kB
func parseMeminfoHugepages(meminfo []byte) map[string]any {
	out := map[string]any{}
	for _, line := range strings.Split(string(meminfo), "\n") {
		name, value, ok := strings.Cut(line, ":")
		if !ok {
			continue
		}
		fields := strings.Fields(value)
		if len(fields) == 0 {
			continue
		}
		v := atoiOrZero(fields[0])
		switch strings.TrimSpace(name) {
		case "HugePages_Total":
			out["total"] = v
		case "HugePages_Free":
			out["free"] = v
		case "HugePages_Rsvd":
			out["reserved"] = v
		case "HugePages_Surp":
			out["surplus"] = v
		case "Hugepagesize":
			out["default_size_kb"] = v
		}
	}
	return out
}

// readSysfsSizes enumerates /sys/kernel/mm/hugepages/hugepages-<size>kB
// directories, one per configured size (2048kB and 1048576kB on x86).
func (c *HugepagesCollector) readSysfsSizes() []hugepageSize {
	base := filepath.Join(c.root(), "sys/kernel/mm/hugepages")
	entries, err := os.ReadDir(base)
	if err != nil {
		return nil
	}

	var sizes []hugepageSize
	for _, e := range entries {
		name := e.Name()
		if !strings.HasPrefix(name, "hugepages-") || !strings.HasSuffix(name, "kB") {
			continue
		}
		kb, err := strconv.Atoi(strings.TrimSuffix(strings.TrimPrefix(name, "hugepages-"), "kB"))
		if err != nil {
			continue
		}
		s := hugepageSize{SizeKB: kb}
		if data, err := os.ReadFile(filepath.Join(base, name, "nr_hugepages")); err == nil {
			s.Nr = atoiOrZero(strings.TrimSpace(string(data)))
		}
		if data, err := os.ReadFile(filepath.Join(base, name, "free_hugepages")); err == nil {
			s.Free = atoiOrZero(strings.TrimSpace(string(data)))
		}
		sizes = append(sizes, s)
	}
	return sizes
}

func dedupeStrings(in []string) []string {
	if len(in) == 0 {
		return nil
	}
	seen := make(map[string]bool, len(in))
	var out []string
	for _, s := range in {
		if seen[s] {
			continue
		}
		seen[s] = true
		out = append(out, s)
	}
	return out
}
