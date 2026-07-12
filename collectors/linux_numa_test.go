package collectors

import (
	"context"
	"slices"
	"testing"
)

const numactlHardwareTwoNode = `available: 2 nodes (0-1)
node 0 cpus: 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 64 65 66 67 68 69 70 71 72 73 74 75 76 77 78 79
node 0 size: 257635 MB
node 0 free: 245001 MB
node 1 cpus: 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 80 81 82 83 84 85 86 87 88 89 90 91 92 93 94 95
node 1 size: 258040 MB
node 1 free: 250112 MB
node distances:
node   0   1
  0:  10  32
  1:  32  10
`

func TestNUMACollectorViaNumactl(t *testing.T) {
	r := newFakeRunner()
	r.set("numactl --hardware", numactlHardwareTwoNode)

	c := &NUMACollector{Runner: r, LookPath: lookPathAll, SysRoot: t.TempDir()}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}

	topo, ok := f.Raw["numa"].(*numaTopology)
	if !ok {
		t.Fatalf("Raw[numa] = %#v", f.Raw["numa"])
	}
	if len(topo.Nodes) != 2 {
		t.Fatalf("nodes = %+v", topo.Nodes)
	}
	if topo.Nodes[0].CPUs != 32 || topo.Nodes[0].SizeMB != 257635 || topo.Nodes[0].FreeMB != 245001 {
		t.Errorf("node 0 = %+v", topo.Nodes[0])
	}
	if topo.Nodes[1].SizeMB != 258040 {
		t.Errorf("node 1 = %+v", topo.Nodes[1])
	}
	wantDist := [][]int{{10, 32}, {32, 10}}
	if len(topo.Distances) != 2 {
		t.Fatalf("distances = %v", topo.Distances)
	}
	for i := range wantDist {
		if !slices.Equal(topo.Distances[i], wantDist[i]) {
			t.Errorf("distances[%d] = %v, want %v", i, topo.Distances[i], wantDist[i])
		}
	}

	if !slices.Contains(f.Capabilities, "multi_numa") {
		t.Errorf("capabilities = %v", f.Capabilities)
	}
}

func TestNUMACollectorSysfsFallback(t *testing.T) {
	root := t.TempDir()
	writeTree(t, root, map[string]string{
		"sys/devices/system/node/node0/cpulist":  "0-63,128-191\n",
		"sys/devices/system/node/node0/distance": "10 32\n",
		"sys/devices/system/node/node0/meminfo":  "Node 0 MemTotal:       263818240 kB\nNode 0 MemFree:        250880000 kB\n",
		"sys/devices/system/node/node1/cpulist":  "64-127,192-255\n",
		"sys/devices/system/node/node1/distance": "32 10\n",
		"sys/devices/system/node/node1/meminfo":  "Node 1 MemTotal:       264241152 kB\nNode 1 MemFree:        256000000 kB\n",
	})

	c := &NUMACollector{LookPath: lookPathNone, SysRoot: root}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}

	topo := f.Raw["numa"].(*numaTopology)
	if len(topo.Nodes) != 2 {
		t.Fatalf("nodes = %+v", topo.Nodes)
	}
	if topo.Nodes[0].CPUs != 128 {
		t.Errorf("node 0 cpus = %d, want 128", topo.Nodes[0].CPUs)
	}
	if topo.Nodes[0].SizeMB != 263818240/1024 {
		t.Errorf("node 0 size = %d", topo.Nodes[0].SizeMB)
	}
	if !slices.Contains(f.Capabilities, "multi_numa") {
		t.Errorf("capabilities = %v", f.Capabilities)
	}
}

func TestNUMACollectorSingleNodeNoCapability(t *testing.T) {
	root := t.TempDir()
	writeTree(t, root, map[string]string{
		"sys/devices/system/node/node0/cpulist":  "0-15\n",
		"sys/devices/system/node/node0/distance": "10\n",
		"sys/devices/system/node/node0/meminfo":  "Node 0 MemTotal:       65536000 kB\n",
	})

	c := &NUMACollector{LookPath: lookPathNone, SysRoot: root}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}
	if len(f.Capabilities) != 0 {
		t.Errorf("single node must not claim multi_numa: %v", f.Capabilities)
	}
}

func TestNUMACollectorNothingReadable(t *testing.T) {
	c := &NUMACollector{LookPath: lookPathNone, SysRoot: t.TempDir()}
	f, err := c.Collect(context.Background(), "srv-1")
	if err == nil {
		t.Fatal("expected error")
	}
	if f == nil {
		t.Fatal("facts must be non-nil even on error")
	}
}

func TestCountCPUList(t *testing.T) {
	cases := map[string]int{
		"0-63,128-191": 128,
		"0":            1,
		"0,2,4":        3,
		"0-3":          4,
		"":             0,
		"7-4":          0,
	}
	for in, want := range cases {
		if got := countCPUList(in); got != want {
			t.Errorf("countCPUList(%q) = %d, want %d", in, got, want)
		}
	}
}
