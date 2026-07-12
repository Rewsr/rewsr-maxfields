package collectors

import (
	"context"
	"slices"
	"testing"
)

func TestIOMMUCollectorEnabledPassthrough(t *testing.T) {
	root := t.TempDir()
	writeTree(t, root, map[string]string{
		"proc/cmdline":                     "BOOT_IMAGE=/boot/vmlinuz-6.8.0-41-generic root=UUID=abc ro amd_iommu=on iommu=pt console=ttyS0\n",
		"sys/kernel/iommu_groups/0/.keep":  "",
		"sys/kernel/iommu_groups/1/.keep":  "",
		"sys/kernel/iommu_groups/17/.keep": "",
		"sys/class/iommu/ivhd0/.keep":      "",
	})

	c := &IOMMUCollector{SysRoot: root}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}

	raw := f.Raw["iommu"].(map[string]any)
	if raw["groups"] != 3 {
		t.Errorf("groups = %v, want 3", raw["groups"])
	}
	flags := raw["cmdline_flags"].([]string)
	if !slices.Contains(flags, "amd_iommu=on") || !slices.Contains(flags, "iommu=pt") {
		t.Errorf("cmdline_flags = %v", flags)
	}

	for _, want := range []string{"iommu", "iommu_passthrough"} {
		if !slices.Contains(f.Capabilities, want) {
			t.Errorf("capabilities missing %q: %v", want, f.Capabilities)
		}
	}
}

func TestIOMMUCollectorDisabled(t *testing.T) {
	root := t.TempDir()
	writeTree(t, root, map[string]string{
		"proc/cmdline": "BOOT_IMAGE=/boot/vmlinuz root=UUID=abc ro\n",
	})

	c := &IOMMUCollector{SysRoot: root}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}
	if len(f.Capabilities) != 0 {
		t.Errorf("no groups must mean no iommu capability: %v", f.Capabilities)
	}
	raw := f.Raw["iommu"].(map[string]any)
	if raw["groups"] != 0 {
		t.Errorf("groups = %v", raw["groups"])
	}
}

func TestIOMMUCollectorGroupsWithoutPassthrough(t *testing.T) {
	root := t.TempDir()
	writeTree(t, root, map[string]string{
		"proc/cmdline":                    "BOOT_IMAGE=/boot/vmlinuz intel_iommu=on\n",
		"sys/kernel/iommu_groups/0/.keep": "",
	})

	c := &IOMMUCollector{SysRoot: root}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}
	if !slices.Contains(f.Capabilities, "iommu") {
		t.Errorf("capabilities = %v", f.Capabilities)
	}
	if slices.Contains(f.Capabilities, "iommu_passthrough") {
		t.Errorf("passthrough not on cmdline: %v", f.Capabilities)
	}
}

func TestIOMMUCollectorNothingReadable(t *testing.T) {
	c := &IOMMUCollector{SysRoot: t.TempDir()}
	f, err := c.Collect(context.Background(), "srv-1")
	if err == nil {
		t.Fatal("expected error")
	}
	if f == nil {
		t.Fatal("facts must be non-nil even on error")
	}
}
