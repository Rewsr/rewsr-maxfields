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

// NUMACollector maps the socket and memory topology of the server: how
// many NUMA nodes, how much memory per node, and the distance matrix
// between them. Fabric data planes pin queues and UMEM to the node that
// owns the NIC, so "which node is close to what" is a placement fact, not
// a tuning detail.
//
// numactl --hardware is the primary source. When numactl is not installed
// the same facts are assembled from /sys/devices/system/node, which is
// always present on a NUMA-aware kernel.
type NUMACollector struct {
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

// NewNUMACollector returns a NUMACollector running commands through r.
func NewNUMACollector(r runner.CommandRunner) *NUMACollector {
	return &NUMACollector{Runner: r, LookPath: exec.LookPath}
}

func (c *NUMACollector) root() string {
	if c.SysRoot == "" {
		return "/"
	}
	return c.SysRoot
}

// numaNode is one NUMA node as carried in Raw["numa"].
type numaNode struct {
	ID     int `json:"id"`
	CPUs   int `json:"cpus"`
	SizeMB int `json:"size_mb"`
	FreeMB int `json:"free_mb,omitempty"`
}

type numaTopology struct {
	Nodes     []numaNode `json:"nodes"`
	Distances [][]int    `json:"distances,omitempty"`
}

// Collect gathers the NUMA topology. A single-node machine is a valid
// answer and produces no capability, just the topology in Raw.
func (c *NUMACollector) Collect(ctx context.Context, serverID string) (*facts.ServerFacts, error) {
	f := &facts.ServerFacts{
		ServerID:   serverID,
		Source:     "linux_numa",
		ObservedAt: time.Now().UTC(),
		Raw:        map[string]any{},
	}

	lookPath := c.LookPath
	if lookPath == nil {
		lookPath = exec.LookPath
	}

	var topo *numaTopology
	if c.Runner != nil && requireCommand(lookPath, "numactl") == nil {
		out, err := c.Runner.Run(ctx, "numactl", "--hardware")
		if err == nil {
			topo = parseNumactlHardware(out)
		}
	}
	if topo == nil || len(topo.Nodes) == 0 {
		topo = c.readSysfsTopology()
	}
	if topo == nil || len(topo.Nodes) == 0 {
		return f, errors.New("numa collector: neither numactl nor " + filepath.Join(c.root(), "sys/devices/system/node") + " usable")
	}

	f.Raw["numa"] = topo
	if len(topo.Nodes) > 1 {
		f.Capabilities = []string{"multi_numa"}
	}

	return f, nil
}

// parseNumactlHardware parses `numactl --hardware`, which looks like:
//
//	available: 2 nodes (0-1)
//	node 0 cpus: 0 1 2 3
//	node 0 size: 257635 MB
//	node 0 free: 245001 MB
//	node 1 cpus: 4 5 6 7
//	node 1 size: 258040 MB
//	node 1 free: 250112 MB
//	node distances:
//	node   0   1
//	  0:  10  32
//	  1:  32  10
func parseNumactlHardware(out []byte) *numaTopology {
	topo := &numaTopology{}
	nodesByID := map[int]*numaNode{}
	inDistances := false

	node := func(id int) *numaNode {
		if n, ok := nodesByID[id]; ok {
			return n
		}
		topo.Nodes = append(topo.Nodes, numaNode{ID: id})
		n := &topo.Nodes[len(topo.Nodes)-1]
		nodesByID[id] = n
		return n
	}

	for _, line := range strings.Split(string(out), "\n") {
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}

		if strings.HasPrefix(line, "node distances:") {
			inDistances = true
			continue
		}

		if inDistances {
			// Skip the header row ("node   0   1"); data rows start with
			// "<id>:".
			id, rest, ok := strings.Cut(line, ":")
			if !ok {
				continue
			}
			if _, err := strconv.Atoi(strings.TrimSpace(id)); err != nil {
				continue
			}
			var row []int
			for _, field := range strings.Fields(rest) {
				if d, err := strconv.Atoi(field); err == nil {
					row = append(row, d)
				}
			}
			if len(row) > 0 {
				topo.Distances = append(topo.Distances, row)
			}
			continue
		}

		if !strings.HasPrefix(line, "node ") {
			continue
		}
		fields := strings.Fields(line)
		if len(fields) < 3 {
			continue
		}
		id, err := strconv.Atoi(fields[1])
		if err != nil {
			continue
		}
		switch strings.TrimSuffix(fields[2], ":") {
		case "cpus":
			node(id).CPUs = len(fields) - 3
		case "size":
			if len(fields) >= 4 {
				node(id).SizeMB = atoiOrZero(fields[3])
			}
		case "free":
			if len(fields) >= 4 {
				node(id).FreeMB = atoiOrZero(fields[3])
			}
		}
	}

	// nodesByID holds pointers into topo.Nodes, which append may have
	// relocated; the values in topo.Nodes themselves are authoritative.
	return topo
}

// readSysfsTopology assembles the same topology from
// /sys/devices/system/node/node<N>: cpulist for the CPU count, meminfo for
// the size, and distance for the matrix row.
func (c *NUMACollector) readSysfsTopology() *numaTopology {
	base := filepath.Join(c.root(), "sys/devices/system/node")
	entries, err := os.ReadDir(base)
	if err != nil {
		return nil
	}

	topo := &numaTopology{}
	var ids []int
	for _, e := range entries {
		name := e.Name()
		if !strings.HasPrefix(name, "node") {
			continue
		}
		id, err := strconv.Atoi(strings.TrimPrefix(name, "node"))
		if err != nil {
			continue
		}
		ids = append(ids, id)
	}
	if len(ids) == 0 {
		return nil
	}
	// ReadDir returns lexical order, which puts node10 before node2;
	// sort numerically so distance rows line up with node ids.
	for i := 0; i < len(ids); i++ {
		for j := i + 1; j < len(ids); j++ {
			if ids[j] < ids[i] {
				ids[i], ids[j] = ids[j], ids[i]
			}
		}
	}

	for _, id := range ids {
		nodeDir := filepath.Join(base, "node"+strconv.Itoa(id))
		n := numaNode{ID: id}

		if data, err := os.ReadFile(filepath.Join(nodeDir, "cpulist")); err == nil {
			n.CPUs = countCPUList(strings.TrimSpace(string(data)))
		}
		if data, err := os.ReadFile(filepath.Join(nodeDir, "meminfo")); err == nil {
			n.SizeMB = parseNodeMeminfoMB(string(data), "MemTotal")
			n.FreeMB = parseNodeMeminfoMB(string(data), "MemFree")
		}
		topo.Nodes = append(topo.Nodes, n)

		if data, err := os.ReadFile(filepath.Join(nodeDir, "distance")); err == nil {
			var row []int
			for _, field := range strings.Fields(string(data)) {
				if d, err := strconv.Atoi(field); err == nil {
					row = append(row, d)
				}
			}
			if len(row) > 0 {
				topo.Distances = append(topo.Distances, row)
			}
		}
	}
	return topo
}

// countCPUList counts CPUs in a kernel cpulist string like "0-63,128-191".
func countCPUList(list string) int {
	if list == "" {
		return 0
	}
	count := 0
	for _, part := range strings.Split(list, ",") {
		lo, hi, isRange := strings.Cut(part, "-")
		if !isRange {
			count++
			continue
		}
		a, errA := strconv.Atoi(strings.TrimSpace(lo))
		b, errB := strconv.Atoi(strings.TrimSpace(hi))
		if errA != nil || errB != nil || b < a {
			continue
		}
		count += b - a + 1
	}
	return count
}

// parseNodeMeminfoMB pulls one field out of a per-node meminfo file, whose
// lines look like "Node 0 MemTotal:       263818240 kB", and converts kB
// to MB.
func parseNodeMeminfoMB(meminfo, field string) int {
	for _, line := range strings.Split(meminfo, "\n") {
		if !strings.Contains(line, field+":") {
			continue
		}
		fields := strings.Fields(line)
		// "Node <id> <field>: <value> kB"
		for i, tok := range fields {
			if strings.HasPrefix(tok, field) && i+1 < len(fields) {
				return atoiOrZero(fields[i+1]) / 1024
			}
		}
	}
	return 0
}
