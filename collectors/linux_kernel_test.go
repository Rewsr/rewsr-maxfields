package collectors

import (
	"context"
	"slices"
	"strings"
	"testing"
)

const procModules = `mlx5_ib 495616 0 - Live 0x0000000000000000
mlx5_core 2211840 1 mlx5_ib, Live 0x0000000000000000
ib_uverbs 188416 2 mlx5_ib,rdma_ucm, Live 0x0000000000000000
ib_core 462848 4 mlx5_ib,ib_uverbs,rdma_ucm,iw_cm, Live 0x0000000000000000
rdma_ucm 32768 0 - Live 0x0000000000000000
vfio_pci 16384 0 - Live 0x0000000000000000
vfio_pci_core 94208 1 vfio_pci, Live 0x0000000000000000
vfio 65536 3 vfio_pci,vfio_pci_core,vfio_iommu_type1, Live 0x0000000000000000
kvm_amd 208896 0 - Live 0x0000000000000000
kvm 1339392 1 kvm_amd, Live 0x0000000000000000
bonding 258048 0 - Live 0x0000000000000000
uninteresting_module 4096 0 - Live 0x0000000000000000
`

func TestKernelCollectorFabricHost(t *testing.T) {
	root := t.TempDir()
	writeTree(t, root, map[string]string{
		"proc/cmdline":                         "BOOT_IMAGE=/boot/vmlinuz-6.8.0-41-generic root=UUID=abc ro amd_iommu=on iommu=pt default_hugepagesz=1G hugepagesz=1G hugepages=16\n",
		"proc/modules":                         procModules,
		"proc/sys/net/core/busy_poll":          "50\n",
		"proc/sys/net/core/busy_read":          "50\n",
		"proc/sys/net/core/rmem_max":           "268435456\n",
		"proc/sys/net/core/wmem_max":           "268435456\n",
		"proc/sys/net/core/netdev_max_backlog": "5000\n",
	})

	r := newFakeRunner()
	r.set("uname -r", "6.8.0-41-generic\n")

	c := &KernelCollector{Runner: r, LookPath: lookPathAll, SysRoot: root}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}

	raw := f.Raw["kernel"].(map[string]any)
	if raw["release"] != "6.8.0-41-generic" {
		t.Errorf("release = %v", raw["release"])
	}
	if cmdline := raw["cmdline"].(string); !strings.Contains(cmdline, "iommu=pt") {
		t.Errorf("cmdline = %q", cmdline)
	}

	modules := raw["modules"].([]string)
	for _, want := range []string{"vfio_pci", "ib_core", "mlx5_ib", "kvm_amd", "bonding"} {
		if !slices.Contains(modules, want) {
			t.Errorf("modules missing %q: %v", want, modules)
		}
	}
	if slices.Contains(modules, "uninteresting_module") {
		t.Errorf("module filter leaked: %v", modules)
	}

	sysctls := raw["sysctls"].(map[string]string)
	if sysctls["net.core.busy_poll"] != "50" || sysctls["net.core.rmem_max"] != "268435456" {
		t.Errorf("sysctls = %v", sysctls)
	}

	if !slices.Contains(f.Capabilities, "vfio") {
		t.Errorf("capabilities = %v", f.Capabilities)
	}
}

func TestKernelCollectorNoVfioNoCapability(t *testing.T) {
	root := t.TempDir()
	writeTree(t, root, map[string]string{
		"proc/modules": "kvm_intel 425984 0 - Live 0x0000000000000000\n",
		"proc/cmdline": "BOOT_IMAGE=/boot/vmlinuz ro\n",
	})

	c := &KernelCollector{LookPath: lookPathNone, SysRoot: root}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}
	if len(f.Capabilities) != 0 {
		t.Errorf("capabilities = %v", f.Capabilities)
	}
	raw := f.Raw["kernel"].(map[string]any)
	if _, present := raw["release"]; present {
		t.Error("release should be absent without uname")
	}
}

func TestKernelCollectorNothingReadable(t *testing.T) {
	c := &KernelCollector{LookPath: lookPathNone, SysRoot: t.TempDir()}
	f, err := c.Collect(context.Background(), "srv-1")
	if err == nil {
		t.Fatal("expected error")
	}
	if f == nil {
		t.Fatal("facts must be non-nil even on error")
	}
}

func TestWave2CollectorsWiring(t *testing.T) {
	list := Wave2Collectors(newFakeRunner())
	if len(list) != 14 {
		t.Fatalf("wave 2 should wire 14 collectors, got %d", len(list))
	}
	seen := map[string]bool{}
	for _, c := range list {
		f, _ := c.Collect(context.Background(), "srv-1")
		if f == nil {
			t.Fatal("collectors must return non-nil facts even on error")
		}
		if f.Source == "" {
			t.Errorf("collector %T has empty source", c)
		}
		if seen[f.Source] {
			t.Errorf("duplicate source %q", f.Source)
		}
		seen[f.Source] = true
	}
}
