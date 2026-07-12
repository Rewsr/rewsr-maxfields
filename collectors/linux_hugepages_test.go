package collectors

import (
	"context"
	"slices"
	"testing"
)

const meminfoWithHugepages = `MemTotal:       527636480 kB
MemFree:        401235968 kB
MemAvailable:   488636416 kB
Buffers:          524288 kB
Cached:         84934656 kB
SwapCached:            0 kB
Active:         52428800 kB
Inactive:       41943040 kB
Mlocked:           16384 kB
SwapTotal:             0 kB
SwapFree:              0 kB
Dirty:              2048 kB
Writeback:             0 kB
AnonPages:       9437184 kB
Mapped:          1048576 kB
Shmem:            262144 kB
KReclaimable:    2097152 kB
Slab:            4194304 kB
VmallocTotal:   34359738367 kB
HugePages_Total:    1024
HugePages_Free:      512
HugePages_Rsvd:       64
HugePages_Surp:        0
Hugepagesize:       2048 kB
Hugetlb:         2097152 kB
`

func TestHugepagesCollectorConfigured(t *testing.T) {
	root := t.TempDir()
	writeTree(t, root, map[string]string{
		"proc/meminfo": meminfoWithHugepages,
		"sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages":      "1024\n",
		"sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages":    "512\n",
		"sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages":   "16\n",
		"sys/kernel/mm/hugepages/hugepages-1048576kB/free_hugepages": "16\n",
	})

	c := &HugepagesCollector{SysRoot: root}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}

	raw, ok := f.Raw["hugepages"].(map[string]any)
	if !ok {
		t.Fatalf("Raw[hugepages] = %#v", f.Raw["hugepages"])
	}
	if raw["total"] != 1024 || raw["free"] != 512 || raw["reserved"] != 64 {
		t.Errorf("counters = %v", raw)
	}
	if raw["default_size_kb"] != 2048 {
		t.Errorf("default_size_kb = %v", raw["default_size_kb"])
	}

	sizes := raw["sizes"].([]hugepageSize)
	if len(sizes) != 2 {
		t.Fatalf("sizes = %+v", sizes)
	}

	for _, want := range []string{"hugepages", "hugepages_1g"} {
		if !slices.Contains(f.Capabilities, want) {
			t.Errorf("capabilities missing %q: %v", want, f.Capabilities)
		}
	}
}

func TestHugepagesCollectorNoneReserved(t *testing.T) {
	root := t.TempDir()
	writeTree(t, root, map[string]string{
		"proc/meminfo": `MemTotal:       32768000 kB
HugePages_Total:       0
HugePages_Free:        0
HugePages_Rsvd:        0
HugePages_Surp:        0
Hugepagesize:       2048 kB
`,
		"sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages":   "0\n",
		"sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages": "0\n",
	})

	c := &HugepagesCollector{SysRoot: root}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}
	if len(f.Capabilities) != 0 {
		t.Errorf("zero reservation must not claim hugepages: %v", f.Capabilities)
	}
}

func TestHugepagesCollectorSysfsOnly(t *testing.T) {
	// meminfo unreadable but sysfs present: still a real reservation.
	root := t.TempDir()
	writeTree(t, root, map[string]string{
		"sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages":   "8\n",
		"sys/kernel/mm/hugepages/hugepages-1048576kB/free_hugepages": "8\n",
	})

	c := &HugepagesCollector{SysRoot: root}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}
	for _, want := range []string{"hugepages", "hugepages_1g"} {
		if !slices.Contains(f.Capabilities, want) {
			t.Errorf("capabilities missing %q: %v", want, f.Capabilities)
		}
	}
}

func TestHugepagesCollectorNothingReadable(t *testing.T) {
	c := &HugepagesCollector{SysRoot: t.TempDir()}
	f, err := c.Collect(context.Background(), "srv-1")
	if err == nil {
		t.Fatal("expected error")
	}
	if f == nil {
		t.Fatal("facts must be non-nil even on error")
	}
}
