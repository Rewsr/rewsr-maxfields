package collectors

import (
	"context"
	"slices"
	"testing"
)

// lspciDnnmmOutput is a trimmed but format-faithful `lspci -Dnnmm` capture
// of a GPU server: host bridges and platform devices that the collector
// must filter out, a ConnectX-6 Dx pair, an H100, and a boot NVMe drive.
const lspciDnnmmOutput = `0000:00:00.0 "Host bridge [0600]" "Advanced Micro Devices, Inc. [AMD] [1022]" "Starship/Matisse Root Complex [1480]" "" "Advanced Micro Devices, Inc. [AMD] [1022]" "Starship/Matisse Root Complex [1480]"
0000:00:01.1 "PCI bridge [0604]" "Advanced Micro Devices, Inc. [AMD] [1022]" "Starship/Matisse GPP Bridge [1483]" "" "" ""
0000:01:00.0 "Non-Volatile memory controller [0108]" "Samsung Electronics Co Ltd [144d]" "NVMe SSD Controller PM9A1/PM9A3/980PRO [a80a]" -r00 "Samsung Electronics Co Ltd [144d]" "PM9A3 [a801]"
0000:41:00.0 "Ethernet controller [0200]" "Mellanox Technologies [15b3]" "MT2892 Family [ConnectX-6 Dx] [101d]" -r00 "Mellanox Technologies [15b3]" "ConnectX-6 Dx SmartNIC [0083]"
0000:41:00.1 "Ethernet controller [0200]" "Mellanox Technologies [15b3]" "MT2892 Family [ConnectX-6 Dx] [101d]" -r00 "Mellanox Technologies [15b3]" "ConnectX-6 Dx SmartNIC [0083]"
0000:c1:00.0 "Processing accelerators [1200]" "NVIDIA Corporation [10de]" "GH100 [H100 SXM5 80GB] [2330]" -ra1 "NVIDIA Corporation [10de]" "GH100 [H100 SXM5 80GB] [16c1]"
0000:c2:00.0 "ISA bridge [0601]" "Advanced Micro Devices, Inc. [AMD] [1022]" "FCH LPC Bridge [790e]" -r51 "" ""
`

func pciSysfsFixture(t *testing.T) string {
	root := t.TempDir()
	writeTree(t, root, map[string]string{
		"sys/bus/pci/devices/0000:01:00.0/current_link_speed": "8.0 GT/s PCIe\n",
		"sys/bus/pci/devices/0000:01:00.0/max_link_speed":     "16.0 GT/s PCIe\n",
		"sys/bus/pci/devices/0000:01:00.0/current_link_width": "4\n",
		"sys/bus/pci/devices/0000:01:00.0/max_link_width":     "4\n",
		"sys/bus/pci/devices/0000:41:00.0/current_link_speed": "16.0 GT/s PCIe\n",
		"sys/bus/pci/devices/0000:41:00.0/max_link_speed":     "16.0 GT/s PCIe\n",
		"sys/bus/pci/devices/0000:41:00.0/current_link_width": "16\n",
		"sys/bus/pci/devices/0000:41:00.0/max_link_width":     "16\n",
		"sys/bus/pci/devices/0000:41:00.0/sriov_totalvfs":     "16\n",
		"sys/bus/pci/devices/0000:41:00.0/sriov_numvfs":       "0\n",
		"sys/bus/pci/devices/0000:c1:00.0/current_link_speed": "32.0 GT/s PCIe\n",
		"sys/bus/pci/devices/0000:c1:00.0/max_link_speed":     "32.0 GT/s PCIe\n",
		"sys/bus/pci/devices/0000:c1:00.0/current_link_width": "16\n",
		"sys/bus/pci/devices/0000:c1:00.0/max_link_width":     "16\n",
	})
	return root
}

func TestPCICollectorGPUServer(t *testing.T) {
	r := newFakeRunner()
	r.set("lspci -Dnnmm", lspciDnnmmOutput)

	c := &PCICollector{Runner: r, LookPath: lookPathAll, SysRoot: pciSysfsFixture(t)}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}

	pci, ok := f.Raw["pci"].(map[string]any)
	if !ok {
		t.Fatalf("Raw[pci] = %#v", f.Raw["pci"])
	}
	if pci["total_functions"] != 7 {
		t.Errorf("total_functions = %v, want 7", pci["total_functions"])
	}

	devices := pci["devices"].([]pciDevice)
	if len(devices) != 4 {
		t.Fatalf("want 4 interesting devices (nvme, 2 nics, gpu), got %d: %+v", len(devices), devices)
	}

	byAddr := map[string]pciDevice{}
	for _, d := range devices {
		byAddr[d.Address] = d
	}

	nic := byAddr["0000:41:00.0"]
	if nic.VendorID != "15b3" || nic.DeviceID != "101d" || nic.ClassID != "0200" {
		t.Errorf("nic ids = %+v", nic)
	}
	if nic.DeviceName != "MT2892 Family [ConnectX-6 Dx]" {
		t.Errorf("nic device name = %q", nic.DeviceName)
	}
	if nic.MaxLinkGTs != 16 || nic.MaxLinkX != 16 || nic.SRIOVTotal != 16 {
		t.Errorf("nic link/sriov = %+v", nic)
	}

	gpu := byAddr["0000:c1:00.0"]
	if gpu.MaxLinkGTs != 32 || gpu.Revision != "a1" {
		t.Errorf("gpu = %+v", gpu)
	}

	nvme := byAddr["0000:01:00.0"]
	if nvme.CurLinkGTs != 8 || nvme.MaxLinkGTs != 16 {
		t.Errorf("nvme link = %+v", nvme)
	}

	for _, want := range []string{"pcie_gen5", "gpu_accelerator", "sriov"} {
		if !slices.Contains(f.Capabilities, want) {
			t.Errorf("capabilities missing %q: %v", want, f.Capabilities)
		}
	}
	if slices.Contains(f.Capabilities, "pcie_gen4") {
		t.Errorf("only the best generation should be reported: %v", f.Capabilities)
	}
}

func TestPCICollectorGen4Only(t *testing.T) {
	r := newFakeRunner()
	r.set("lspci -Dnnmm", `0000:41:00.0 "Ethernet controller [0200]" "Mellanox Technologies [15b3]" "MT2892 Family [ConnectX-6 Dx] [101d]" -r00 "Mellanox Technologies [15b3]" "ConnectX-6 Dx SmartNIC [0083]"`)

	root := t.TempDir()
	writeTree(t, root, map[string]string{
		"sys/bus/pci/devices/0000:41:00.0/max_link_speed": "16.0 GT/s PCIe\n",
	})

	c := &PCICollector{Runner: r, LookPath: lookPathAll, SysRoot: root}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}
	if !slices.Contains(f.Capabilities, "pcie_gen4") || slices.Contains(f.Capabilities, "pcie_gen5") {
		t.Errorf("capabilities = %v", f.Capabilities)
	}
}

func TestPCICollectorMissingLspci(t *testing.T) {
	c := &PCICollector{Runner: newFakeRunner(), LookPath: lookPathNone}
	f, err := c.Collect(context.Background(), "srv-1")
	if err == nil {
		t.Fatal("expected error without lspci")
	}
	if f == nil {
		t.Fatal("facts must be non-nil even on error")
	}
}

func TestSplitLspciFields(t *testing.T) {
	fields, tokens := splitLspciFields(`"Ethernet controller [0200]" "Mellanox Technologies [15b3]" "MT2892 Family [ConnectX-6 Dx] [101d]" -r00 -p00 "Mellanox Technologies [15b3]" ""`)
	if len(fields) != 5 {
		t.Fatalf("fields = %q", fields)
	}
	if fields[2] != "MT2892 Family [ConnectX-6 Dx] [101d]" {
		t.Errorf("field 2 = %q", fields[2])
	}
	if len(tokens) != 2 || tokens[0] != "-r00" || tokens[1] != "-p00" {
		t.Errorf("tokens = %q", tokens)
	}
	if fields[4] != "" {
		t.Errorf("empty quoted field should stay empty, got %q", fields[4])
	}
}

func TestSplitNameAndID(t *testing.T) {
	cases := []struct {
		in, name, id string
	}{
		{"Ethernet controller [0200]", "Ethernet controller", "0200"},
		{"MT2892 Family [ConnectX-6 Dx] [101d]", "MT2892 Family [ConnectX-6 Dx]", "101d"},
		{"", "", ""},
		{"NoBrackets", "NoBrackets", ""},
	}
	for _, tc := range cases {
		name, id := splitNameAndID(tc.in)
		if name != tc.name || id != tc.id {
			t.Errorf("splitNameAndID(%q) = %q, %q; want %q, %q", tc.in, name, id, tc.name, tc.id)
		}
	}
}
