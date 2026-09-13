package collectors

import (
	"context"
	"encoding/json"
	"errors"
	"os/exec"
	"time"

	"github.com/Rewsr/rewsr-maxfields/facts"
	"github.com/Rewsr/rewsr-maxfields/runner"
)

// NVMeCollector inventories NVMe controllers through nvme-cli: model,
// firmware revision, serial, and capacity per device. The basic disk scan
// already reports NVMe block devices and sizes; this adds the identity
// layer needed to answer "which of our three PM9A3 firmware revisions is
// this box on", which is the question during a firmware-correlated
// latency hunt.
type NVMeCollector struct {
	Runner runner.CommandRunner

	// LookPath resolves a command name to a path, or errors if the
	// command isn't installed. Defaults to exec.LookPath; overridable in
	// tests.
	LookPath func(file string) (string, error)
}

// NewNVMeCollector returns an NVMeCollector running commands through r.
func NewNVMeCollector(r runner.CommandRunner) *NVMeCollector {
	return &NVMeCollector{Runner: r, LookPath: exec.LookPath}
}

// nvmeDevice is one namespace as carried in Raw["nvme"].
type nvmeDevice struct {
	DevicePath string `json:"device_path"`
	Model      string `json:"model"`
	Firmware   string `json:"firmware"`
	Serial     string `json:"serial"`
	SizeGB     int    `json:"size_gb"`
}

// nvmeListJSON mirrors the fields of `nvme list -o json` this collector
// needs.
type nvmeListJSON struct {
	Devices []struct {
		DevicePath   string `json:"DevicePath"`
		ModelNumber  string `json:"ModelNumber"`
		Firmware     string `json:"Firmware"`
		SerialNumber string `json:"SerialNumber"`
		PhysicalSize int64  `json:"PhysicalSize"`
	} `json:"Devices"`
}

// Collect inventories NVMe namespaces. A host with nvme-cli installed and
// zero NVMe devices is a valid empty answer.
func (c *NVMeCollector) Collect(ctx context.Context, serverID string) (*facts.ServerFacts, error) {
	f := &facts.ServerFacts{
		ServerID:   serverID,
		Source:     "linux_nvme",
		ObservedAt: time.Now().UTC(),
		Raw:        map[string]any{},
	}

	if c.Runner == nil {
		return f, errors.New("nvme collector: no CommandRunner configured")
	}
	lookPath := c.LookPath
	if lookPath == nil {
		lookPath = exec.LookPath
	}
	if err := requireCommand(lookPath, "nvme"); err != nil {
		return f, errors.New("nvme collector: " + err.Error())
	}

	out, err := c.Runner.Run(ctx, "nvme", "list", "-o", "json")
	if err != nil {
		return f, errors.New("nvme collector: " + err.Error())
	}

	var parsed nvmeListJSON
	if err := json.Unmarshal(out, &parsed); err != nil {
		return f, errors.New("nvme collector: parsing nvme list: " + err.Error())
	}
	if len(parsed.Devices) == 0 {
		return f, nil
	}

	devices := make([]nvmeDevice, 0, len(parsed.Devices))
	for _, d := range parsed.Devices {
		devices = append(devices, nvmeDevice{
			DevicePath: d.DevicePath,
			Model:      d.ModelNumber,
			Firmware:   d.Firmware,
			Serial:     d.SerialNumber,
			SizeGB:     int(d.PhysicalSize / (1000 * 1000 * 1000)),
		})
	}
	f.Raw["nvme"] = devices

	return f, nil
}
