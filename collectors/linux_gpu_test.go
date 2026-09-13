package collectors

import (
	"context"
	"slices"
	"testing"
)

func TestGPUCollectorNvidiaInferenceHost(t *testing.T) {
	root := t.TempDir()
	writeTree(t, root, map[string]string{
		"sys/bus/pci/devices/0000:17:00.0/vendor": "0x10de\n",
		"sys/bus/pci/devices/0000:17:00.0/device": "0x2330\n",
		"sys/bus/pci/devices/0000:17:00.0/class":  "0x030200\n",
		"dev/nvidiactl":                           "",
		"proc/modules":                            "nvidia_peermem 16384 0 - Live 0x0000000000000000\n",
		"sys/class/infiniband/mlx5_0/fw_ver":      "28.39.2048\n",
	})

	r := newFakeRunner()
	r.set("nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv,noheader",
		"NVIDIA H100 80GB HBM3, 550.54.15, 81559 MiB\n")

	c := &GPUCollector{Runner: r, LookPath: lookPathAll, SysRoot: root}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}

	for _, want := range []string{"gpu", "nvidia_gpu", "gpu_cuda", "gpudirect_rdma"} {
		if !slices.Contains(f.Capabilities, want) {
			t.Errorf("capabilities missing %q: %v", want, f.Capabilities)
		}
	}
	if slices.Contains(f.Capabilities, "bluefield_dpu") {
		t.Errorf("bluefield_dpu should not be reported: %v", f.Capabilities)
	}
	if f.Source != "linux_gpu" {
		t.Errorf("source = %q", f.Source)
	}
	if _, ok := f.Raw["gpu"]; !ok {
		t.Error("Raw[gpu] missing")
	}
}

func TestGPUCollectorBlueFieldDPU(t *testing.T) {
	root := t.TempDir()
	writeTree(t, root, map[string]string{
		"sys/bus/pci/devices/0000:03:00.0/vendor": "0x15b3\n",
		"sys/bus/pci/devices/0000:03:00.0/device": "0xa2dc\n",
		"sys/bus/pci/devices/0000:03:00.0/class":  "0x020000\n",
		"dev/mst/placeholder":                     "",
	})

	c := &GPUCollector{Runner: newFakeRunner(), LookPath: lookPathNone, SysRoot: root}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}
	if !slices.Contains(f.Capabilities, "bluefield_dpu") {
		t.Errorf("bluefield_dpu missing: %v", f.Capabilities)
	}
	if !slices.Contains(f.Capabilities, "doca") {
		t.Errorf("doca missing (mst present): %v", f.Capabilities)
	}
	if slices.Contains(f.Capabilities, "nvidia_gpu") {
		t.Errorf("nvidia_gpu should not be reported on a plain DPU host: %v", f.Capabilities)
	}
}

func TestGPUCollectorNoGPUDirectWithoutRDMA(t *testing.T) {
	// peermem loaded but no RDMA device: GPUDirect RDMA is not the fact.
	root := t.TempDir()
	writeTree(t, root, map[string]string{
		"sys/bus/pci/devices/0000:17:00.0/vendor": "0x10de\n",
		"sys/bus/pci/devices/0000:17:00.0/device": "0x2330\n",
		"sys/bus/pci/devices/0000:17:00.0/class":  "0x030000\n",
		"proc/modules":                            "nvidia_peermem 16384 0 - Live 0x0000000000000000\n",
	})

	c := &GPUCollector{Runner: newFakeRunner(), LookPath: lookPathNone, SysRoot: root}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}
	if !slices.Contains(f.Capabilities, "nvidia_gpu") {
		t.Errorf("nvidia_gpu missing: %v", f.Capabilities)
	}
	if slices.Contains(f.Capabilities, "gpudirect_rdma") {
		t.Errorf("gpudirect_rdma reported without an RDMA device: %v", f.Capabilities)
	}
}

func TestGPUCollectorNoSources(t *testing.T) {
	c := &GPUCollector{Runner: newFakeRunner(), LookPath: lookPathNone, SysRoot: t.TempDir()}
	f, err := c.Collect(context.Background(), "srv-1")
	if err == nil {
		t.Fatal("expected error when nothing is readable")
	}
	if f == nil {
		t.Fatal("facts must be non-nil even on error")
	}
	if len(f.Capabilities) != 0 {
		t.Errorf("no capabilities expected: %v", f.Capabilities)
	}
}

func TestParseNvidiaSMICSV(t *testing.T) {
	out := "NVIDIA H100 80GB HBM3, 550.54.15, 81559 MiB\nNVIDIA H100 80GB HBM3, 550.54.15, 81559 MiB\n"
	gpus := parseNvidiaSMICSV([]byte(out))
	if len(gpus) != 2 {
		t.Fatalf("expected 2 GPUs, got %d", len(gpus))
	}
	if gpus[0].Name != "NVIDIA H100 80GB HBM3" {
		t.Errorf("name = %q", gpus[0].Name)
	}
	if gpus[0].DriverVersion != "550.54.15" {
		t.Errorf("driver = %q", gpus[0].DriverVersion)
	}
	if gpus[0].MemoryTotal != "81559 MiB" {
		t.Errorf("memory = %q", gpus[0].MemoryTotal)
	}
}
