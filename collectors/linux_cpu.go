package collectors

import (
	"context"
	"encoding/json"
	"strconv"
	"strings"

	"github.com/Rewsr/rewsr-facts/facts"
)

// lscpuJSON mirrors the shape of `lscpu -J` output: a flat list of
// field/data pairs rather than a nested object, so we look fields up by
// name instead of unmarshaling into named struct fields directly.
type lscpuJSON struct {
	Lscpu []struct {
		Field string `json:"field"`
		Data  string `json:"data"`
	} `json:"lscpu"`
}

func (c *LinuxHostCollector) collectCPU(ctx context.Context, lookPath func(string) (string, error), f *facts.ServerFacts) error {
	if err := requireCommand(lookPath, "lscpu"); err != nil {
		return err
	}

	out, err := c.Runner.Run(ctx, "lscpu", "-J")
	if err != nil {
		return err
	}

	var parsed lscpuJSON
	if err := json.Unmarshal(out, &parsed); err != nil {
		return err
	}

	fields := make(map[string]string, len(parsed.Lscpu))
	for _, entry := range parsed.Lscpu {
		fields[strings.TrimSuffix(entry.Field, ":")] = entry.Data
	}

	f.CPU.Vendor = fields["Vendor ID"]
	f.CPU.Model = fields["Model name"]
	f.CPU.Sockets = atoiOrZero(fields["Socket(s)"])
	coresPerSocket := atoiOrZero(fields["Core(s) per socket"])
	if f.CPU.Sockets > 0 && coresPerSocket > 0 {
		f.CPU.Cores = f.CPU.Sockets * coresPerSocket
	}
	// "CPU(s):" is lscpu's total logical processor count, i.e. total
	// hardware threads across all sockets.
	f.CPU.Threads = atoiOrZero(fields["CPU(s)"])

	return nil
}

func atoiOrZero(s string) int {
	n, err := strconv.Atoi(strings.TrimSpace(s))
	if err != nil {
		return 0
	}
	return n
}
