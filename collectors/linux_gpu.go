package collectors

import (
	"context"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"

	"github.com/Rewsr/rewsr-facts/facts"
	"github.com/Rewsr/rewsr-facts/runner"
)

// GPUCollector detects GPU and DPU capability on the host: NVIDIA CUDA GPUs
// used for LLM inference serving, Mellanox/NVIDIA BlueField DPUs and their
// DOCA tooling, and the GPUDirect RDMA path that lets a NIC DMA straight
// into GPU memory. Rewsr places inference and DOCA workloads onto bare
// metal, so "can this server actually run CUDA / DOCA / GPUDirect" is a
// scheduling fact rather than something guessed from a SKU string.
//
// Sources are cheap and dependency-free where possible: PCI identity from
// /sys/bus/pci, loaded modules from /proc/modules, device nodes from /dev,
// and nvidia-smi only when the binary is actually installed. Every command
// and file read degrades to "not present" instead of failing the collector;
// the only error is a host where not a single source was readable.
type GPUCollector struct {
	Runner runner.CommandRunner

	// LookPath resolves a command name to a path, or errors if the
	// command isn't installed. Defaults to exec.LookPath; overridable in
	// tests.
	LookPath func(file string) (string, error)

	// SysRoot is prepended to every /sys, /proc, and /dev path this
	// collector reads. The zero value means the real filesystem root;
	// tests point it at a fixture tree instead.
	SysRoot string
}

// NewGPUCollector returns a GPUCollector running commands through r.
func NewGPUCollector(r runner.CommandRunner) *GPUCollector {
	return &GPUCollector{Runner: r, LookPath: exec.LookPath}
}

func (c *GPUCollector) root() string {
	if c.SysRoot == "" {
		return "/"
	}
	return c.SysRoot
}

// gpuDevice is one accelerator or DPU PCI function as carried in Raw["gpu"].
type gpuDevice struct {
	Address  string `json:"address"`
	VendorID string `json:"vendor_id"`
	DeviceID string `json:"device_id"`
	ClassID  string `json:"class_id"`
	Kind     string `json:"kind"`
}

// nvidiaSMIGpu is one GPU line from nvidia-smi, when the tool is present.
type nvidiaSMIGpu struct {
	Name          string `json:"name"`
	DriverVersion string `json:"driver_version,omitempty"`
	MemoryTotal   string `json:"memory_total,omitempty"`
}

// PCI vendor ids we care about.
const (
	pciVendorNVIDIA   = "0x10de"
	pciVendorMellanox = "0x15b3"
)

// bluefieldDeviceIDs are the Mellanox/NVIDIA BlueField DPU PCI device ids
// (BlueField, BlueField-2, BlueField-3). Plain ConnectX NICs share vendor
// 0x15b3, so a DPU is identified by device id, not vendor alone.
var bluefieldDeviceIDs = map[string]bool{
	"0xa2d2": true, // BlueField
	"0xa2d6": true, // BlueField-2
	"0xa2dc": true, // BlueField-3
}

// Collect gathers GPU and DPU facts. Capability strings are only added for
// features actually observed on the host, never for "the platform could
// support this in theory".
func (c *GPUCollector) Collect(ctx context.Context, serverID string) (*facts.ServerFacts, error) {
	f := &facts.ServerFacts{
		ServerID:   serverID,
		Source:     "linux_gpu",
		ObservedAt: time.Now().UTC(),
		Raw:        map[string]any{},
	}

	lookPath := c.LookPath
	if lookPath == nil {
		lookPath = exec.LookPath
	}

	sawAnySource := false

	// PCI scan: NVIDIA display/3D controllers and BlueField DPUs.
	devices, sawPCI := c.scanPCI()
	if sawPCI {
		sawAnySource = true
	}

	nvidiaGPU, bluefield := false, false
	for _, d := range devices {
		switch d.Kind {
		case "nvidia_gpu":
			nvidiaGPU = true
		case "bluefield_dpu":
			bluefield = true
		}
	}

	// nvidia-smi confirms a working CUDA driver stack, which the bare PCI
	// identity cannot: a GPU with no driver loaded is not schedulable for
	// inference. Absent binary or non-zero exit just means "no CUDA here".
	var smiGPUs []nvidiaSMIGpu
	if c.Runner != nil && requireCommand(lookPath, "nvidia-smi") == nil {
		if out, err := c.Runner.Run(ctx, "nvidia-smi", "--query-gpu=name,driver_version,memory.total", "--format=csv,noheader"); err == nil {
			sawAnySource = true
			smiGPUs = parseNvidiaSMICSV(out)
			if len(smiGPUs) > 0 {
				nvidiaGPU = true
			}
		}
	}

	// GPUDirect RDMA needs the peer-memory module bound to the GPU driver
	// and a real RDMA device for the NIC side. Either half alone is not the
	// capability.
	peermem, sawModules := c.hasPeermemModule()
	if sawModules {
		sawAnySource = true
	}
	rdmaPresent := c.hasRDMADevice()
	if rdmaPresent {
		sawAnySource = true
	}

	// DOCA / BlueField tooling: mst devices from mstflint/OFED, or a doca
	// binary on PATH. Either indicates the DPU management stack is present.
	mstPresent := c.hasMST()
	if mstPresent {
		sawAnySource = true
	}
	docaTool := requireCommand(lookPath, "doca") == nil

	// A CUDA-capable stack means the driver control node exists or nvidia-smi
	// enumerated a GPU.
	cudaReady := len(smiGPUs) > 0 || c.hasNvidiaCtl()
	if c.hasNvidiaCtl() {
		sawAnySource = true
	}

	if !sawAnySource {
		return f, errors.New("gpu collector: no GPU-related sources readable under " + c.root())
	}

	var caps []string
	addCap := func(name string, present bool) {
		if present {
			caps = append(caps, name)
		}
	}
	addCap("gpu", nvidiaGPU)
	addCap("nvidia_gpu", nvidiaGPU)
	addCap("gpu_cuda", cudaReady)
	addCap("bluefield_dpu", bluefield)
	addCap("doca", mstPresent || docaTool)
	addCap("gpudirect_rdma", peermem && rdmaPresent)
	f.Capabilities = caps

	gpuRaw := map[string]any{}
	if len(devices) > 0 {
		gpuRaw["pci"] = devices
	}
	if len(smiGPUs) > 0 {
		gpuRaw["nvidia_smi"] = smiGPUs
	}
	if peermem {
		gpuRaw["peermem_module"] = true
	}
	if len(gpuRaw) > 0 {
		f.Raw["gpu"] = gpuRaw
	}

	return f, nil
}

// scanPCI walks /sys/bus/pci/devices reading each function's vendor, device,
// and class. It returns the NVIDIA display/3D controllers and BlueField DPUs
// found, and whether the sysfs tree was readable at all. A missing tree (a
// non-Linux dev machine) is not an error, just "no PCI facts".
func (c *GPUCollector) scanPCI() (devices []gpuDevice, sawSource bool) {
	base := filepath.Join(c.root(), "sys/bus/pci/devices")
	entries, err := os.ReadDir(base)
	if err != nil {
		return nil, false
	}
	sawSource = true
	for _, e := range entries {
		dir := filepath.Join(base, e.Name())
		vendor := readTrimmedLower(filepath.Join(dir, "vendor"))
		device := readTrimmedLower(filepath.Join(dir, "device"))
		class := readTrimmedLower(filepath.Join(dir, "class"))
		if vendor == "" || class == "" {
			continue
		}
		// PCI class is "0xCCSSPP"; the top byte is the base class. Display
		// controllers are base class 0x03 (VGA 0300, 3D 0302).
		baseClass := ""
		if strings.HasPrefix(class, "0x") && len(class) >= 4 {
			baseClass = class[2:4]
		}
		switch {
		case vendor == pciVendorNVIDIA && baseClass == "03":
			devices = append(devices, gpuDevice{
				Address: e.Name(), VendorID: vendor, DeviceID: device, ClassID: class, Kind: "nvidia_gpu",
			})
		case vendor == pciVendorMellanox && bluefieldDeviceIDs[device]:
			devices = append(devices, gpuDevice{
				Address: e.Name(), VendorID: vendor, DeviceID: device, ClassID: class, Kind: "bluefield_dpu",
			})
		}
	}
	return devices, sawSource
}

// hasPeermemModule reports whether nvidia_peermem or the older nv_peer_mem
// module is loaded, per /proc/modules. The second return is whether the file
// was readable at all.
func (c *GPUCollector) hasPeermemModule() (present, sawSource bool) {
	data, err := os.ReadFile(filepath.Join(c.root(), "proc/modules"))
	if err != nil {
		return false, false
	}
	for _, line := range strings.Split(string(data), "\n") {
		name, _, _ := strings.Cut(line, " ")
		if name == "nvidia_peermem" || name == "nv_peer_mem" {
			return true, true
		}
	}
	return false, true
}

// hasRDMADevice reports whether the kernel RDMA subsystem knows any device,
// via /sys/class/infiniband. This is the NIC side of GPUDirect RDMA.
func (c *GPUCollector) hasRDMADevice() bool {
	entries, err := os.ReadDir(filepath.Join(c.root(), "sys/class/infiniband"))
	if err != nil {
		return false
	}
	return len(entries) > 0
}

// hasMST reports whether /dev/mst exists, which mstflint and NVIDIA OFED
// create for DPU and NIC management. A DOCA host almost always has it.
func (c *GPUCollector) hasMST() bool {
	if _, err := os.Stat(filepath.Join(c.root(), "dev/mst")); err == nil {
		return true
	}
	return false
}

// hasNvidiaCtl reports whether the NVIDIA driver control node exists, which
// means the kernel driver is loaded and the GPU is usable for CUDA.
func (c *GPUCollector) hasNvidiaCtl() bool {
	if _, err := os.Stat(filepath.Join(c.root(), "dev/nvidiactl")); err == nil {
		return true
	}
	return false
}

// parseNvidiaSMICSV parses `nvidia-smi --query-gpu=... --format=csv,noheader`
// output: one GPU per line, comma-separated in query order (name, driver
// version, total memory).
func parseNvidiaSMICSV(out []byte) []nvidiaSMIGpu {
	var gpus []nvidiaSMIGpu
	for _, line := range strings.Split(string(out), "\n") {
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		fields := strings.Split(line, ",")
		g := nvidiaSMIGpu{Name: strings.TrimSpace(fields[0])}
		if g.Name == "" {
			continue
		}
		if len(fields) > 1 {
			g.DriverVersion = strings.TrimSpace(fields[1])
		}
		if len(fields) > 2 {
			g.MemoryTotal = strings.TrimSpace(fields[2])
		}
		gpus = append(gpus, g)
	}
	return gpus
}

// readTrimmedLower reads a small sysfs file and returns its trimmed,
// lowercased contents, or "" on any error. sysfs vendor/device/class ids are
// lowercase hex already, but lowercasing keeps comparisons robust.
func readTrimmedLower(path string) string {
	data, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	return strings.ToLower(strings.TrimSpace(string(data)))
}
