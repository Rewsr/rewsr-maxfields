package collectors

import (
	"context"
	"testing"
)

const nvmeListJSONOutput = `{
  "Devices": [
    {
      "NameSpace": 1,
      "DevicePath": "/dev/nvme0n1",
      "Firmware": "GDC7302Q",
      "Index": 0,
      "ModelNumber": "SAMSUNG MZQL21T9HCJR-00A07",
      "ProductName": "Unknown device",
      "SerialNumber": "S64FNE0R500001",
      "UsedBytes": 494927872,
      "MaximumLBA": 3750748848,
      "PhysicalSize": 1920383410176,
      "SectorSize": 512
    },
    {
      "NameSpace": 1,
      "DevicePath": "/dev/nvme1n1",
      "Firmware": "GDC7302Q",
      "Index": 1,
      "ModelNumber": "SAMSUNG MZQL21T9HCJR-00A07",
      "ProductName": "Unknown device",
      "SerialNumber": "S64FNE0R500002",
      "UsedBytes": 0,
      "MaximumLBA": 3750748848,
      "PhysicalSize": 1920383410176,
      "SectorSize": 512
    }
  ]
}`

func TestNVMeCollector(t *testing.T) {
	r := newFakeRunner()
	r.set("nvme list -o json", nvmeListJSONOutput)

	c := &NVMeCollector{Runner: r, LookPath: lookPathAll}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}

	devices, ok := f.Raw["nvme"].([]nvmeDevice)
	if !ok || len(devices) != 2 {
		t.Fatalf("Raw[nvme] = %#v", f.Raw["nvme"])
	}
	d := devices[0]
	if d.DevicePath != "/dev/nvme0n1" || d.Model != "SAMSUNG MZQL21T9HCJR-00A07" {
		t.Errorf("device 0 = %+v", d)
	}
	if d.Firmware != "GDC7302Q" || d.Serial != "S64FNE0R500001" {
		t.Errorf("device 0 identity = %+v", d)
	}
	if d.SizeGB != 1920 {
		t.Errorf("device 0 size = %d, want 1920", d.SizeGB)
	}
}

func TestNVMeCollectorNoDevices(t *testing.T) {
	r := newFakeRunner()
	r.set("nvme list -o json", `{"Devices":[]}`)

	c := &NVMeCollector{Runner: r, LookPath: lookPathAll}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}
	if _, present := f.Raw["nvme"]; present {
		t.Error("Raw[nvme] should be absent with zero devices")
	}
}

func TestNVMeCollectorMissingTool(t *testing.T) {
	c := &NVMeCollector{Runner: newFakeRunner(), LookPath: lookPathNone}
	if _, err := c.Collect(context.Background(), "srv-1"); err == nil {
		t.Fatal("expected error without nvme-cli")
	}
}
