package collectors

import (
	"context"
	"testing"
)

func TestSMBIOSCollectorRackServer(t *testing.T) {
	root := t.TempDir()
	writeTree(t, root, map[string]string{
		"sys/class/dmi/id/sys_vendor":     "Dell Inc.\n",
		"sys/class/dmi/id/product_name":   "PowerEdge R7525\n",
		"sys/class/dmi/id/product_serial": "CN0ABCDE\n",
		"sys/class/dmi/id/board_vendor":   "Dell Inc.\n",
		"sys/class/dmi/id/board_name":     "0YHMCJ\n",
		"sys/class/dmi/id/bios_vendor":    "Dell Inc.\n",
		"sys/class/dmi/id/bios_version":   "2.15.2\n",
		"sys/class/dmi/id/bios_date":      "01/02/2025\n",
		"sys/class/dmi/id/chassis_type":   "23\n",
	})

	c := &SMBIOSCollector{SysRoot: root}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}

	dmi, ok := f.Raw["dmi"].(map[string]any)
	if !ok {
		t.Fatalf("Raw[dmi] missing or wrong type: %#v", f.Raw["dmi"])
	}
	for field, want := range map[string]string{
		"sys_vendor":        "Dell Inc.",
		"product_name":      "PowerEdge R7525",
		"product_serial":    "CN0ABCDE",
		"bios_version":      "2.15.2",
		"chassis_type":      "23",
		"chassis_type_name": "Rack Mount Chassis",
	} {
		if got := dmi[field]; got != want {
			t.Errorf("dmi[%q] = %v, want %q", field, got, want)
		}
	}
	if f.Source != "linux_smbios" {
		t.Errorf("source = %q", f.Source)
	}
}

func TestSMBIOSCollectorRootOnlyFilesSkipped(t *testing.T) {
	// product_serial absent entirely models the unprivileged case where
	// the read fails; the collector must keep the rest.
	root := t.TempDir()
	writeTree(t, root, map[string]string{
		"sys/class/dmi/id/sys_vendor":   "Supermicro\n",
		"sys/class/dmi/id/product_name": "SYS-120U-TNR\n",
		"sys/class/dmi/id/chassis_type": "17\n",
	})

	c := &SMBIOSCollector{SysRoot: root}
	f, err := c.Collect(context.Background(), "srv-2")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}
	dmi := f.Raw["dmi"].(map[string]any)
	if _, present := dmi["product_serial"]; present {
		t.Error("product_serial should be absent")
	}
	if dmi["chassis_type_name"] != "Main Server Chassis" {
		t.Errorf("chassis_type_name = %v", dmi["chassis_type_name"])
	}
}

func TestSMBIOSCollectorMissingDMIDir(t *testing.T) {
	c := &SMBIOSCollector{SysRoot: t.TempDir()}
	f, err := c.Collect(context.Background(), "srv-3")
	if err == nil {
		t.Fatal("expected error when dmi/id is absent")
	}
	if f == nil {
		t.Fatal("facts must be non-nil even on error")
	}
}

func TestSMBIOSCollectorEmptyValuesOmitted(t *testing.T) {
	root := t.TempDir()
	writeTree(t, root, map[string]string{
		"sys/class/dmi/id/sys_vendor":   "  \n",
		"sys/class/dmi/id/product_name": "ProLiant DL385 Gen11\n",
	})

	c := &SMBIOSCollector{SysRoot: root}
	f, err := c.Collect(context.Background(), "srv-4")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}
	dmi := f.Raw["dmi"].(map[string]any)
	if _, present := dmi["sys_vendor"]; present {
		t.Error("whitespace-only sys_vendor should be omitted")
	}
	if dmi["product_name"] != "ProLiant DL385 Gen11" {
		t.Errorf("product_name = %v", dmi["product_name"])
	}
}
