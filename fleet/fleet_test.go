package fleet

import (
	"encoding/json"
	"slices"
	"testing"

	"github.com/Rewsr/rewsr-maxfields/facts"
)

func sampleFleet() []*facts.ServerFacts {
	return []*facts.ServerFacts{
		{
			ServerID:   "srv-a",
			Health:     "healthy",
			PowerState: "on",
			CPU:        facts.CPUFacts{Model: "AMD EPYC 7763 64-Core Processor", Cores: 128, Threads: 256},
			MemoryGB:   512,
			Disks: []facts.DiskFacts{
				{Name: "nvme0n1", Type: "nvme", SizeGB: 1920},
				{Name: "nvme1n1", Type: "nvme", SizeGB: 1920},
			},
			NICs: []facts.NICFacts{
				{Name: "ens1f0", SpeedGbps: 100},
				{Name: "ens1f1", SpeedGbps: 100},
			},
			Capabilities:  []string{"local_nvme", "rdma", "sriov", "hugepages"},
			NetworkModes:  []string{"rdma", "bonded"},
			FailureDomain: "rack-7",
			Raw: map[string]any{
				"nvme": []map[string]any{
					{"device_path": "/dev/nvme0n1", "model": "SAMSUNG MZQL21T9HCJR-00A07", "firmware": "GDC7302Q", "serial": "S1", "size_gb": 1920},
					{"device_path": "/dev/nvme1n1", "model": "SAMSUNG MZQL21T9HCJR-00A07", "firmware": "GDC7302Q", "serial": "S2", "size_gb": 1920},
				},
				"nic_offload": map[string]any{
					"ens1f0": map[string]any{"driver": map[string]any{"driver": "mlx5_core", "firmware-version": "22.36.1010"}},
					"ens1f1": map[string]any{"driver": map[string]any{"driver": "mlx5_core", "firmware-version": "22.36.1010"}},
				},
			},
		},
		{
			ServerID:      "srv-b",
			Health:        "healthy",
			PowerState:    "on",
			CPU:           facts.CPUFacts{Model: "AMD EPYC 7763 64-Core Processor", Cores: 128, Threads: 256},
			MemoryGB:      512,
			NICs:          []facts.NICFacts{{Name: "eth0", SpeedGbps: 25}},
			Capabilities:  []string{"local_nvme", "hugepages"},
			FailureDomain: "rack-7",
			Raw: map[string]any{
				"nvme": []map[string]any{
					{"device_path": "/dev/nvme0n1", "model": "SAMSUNG MZQL21T9HCJR-00A07", "firmware": "GDC7304Q", "serial": "S3", "size_gb": 1920},
				},
			},
		},
		{
			ServerID:   "srv-c",
			Health:     "degraded",
			PowerState: "off",
			CPU:        facts.CPUFacts{Model: "Intel(R) Xeon(R) Platinum 8480+", Cores: 112, Threads: 224},
			MemoryGB:   1024,
		},
		nil,
	}
}

func TestAggregate(t *testing.T) {
	s := Aggregate(sampleFleet())

	if s.Servers != 3 {
		t.Errorf("servers = %d (nil entry must be skipped)", s.Servers)
	}
	if s.Healthy != 2 || s.Degraded != 1 || s.Unknown != 0 {
		t.Errorf("health = %d/%d/%d", s.Healthy, s.Degraded, s.Unknown)
	}
	if s.PoweredOn != 2 {
		t.Errorf("powered on = %d", s.PoweredOn)
	}
	if s.TotalCores != 368 || s.TotalMemoryGB != 2048 {
		t.Errorf("capacity = %d cores %d GB", s.TotalCores, s.TotalMemoryGB)
	}
	if s.TotalDiskGB != 3840 {
		t.Errorf("disk = %d", s.TotalDiskGB)
	}
	if s.TotalNICGbps != 225 {
		t.Errorf("nic gbps = %d", s.TotalNICGbps)
	}
	if s.NICSpeedPorts[100] != 2 || s.NICSpeedPorts[25] != 1 {
		t.Errorf("nic ports = %v", s.NICSpeedPorts)
	}
	if s.CapabilityCounts["local_nvme"] != 2 || s.CapabilityCounts["rdma"] != 1 {
		t.Errorf("capabilities = %v", s.CapabilityCounts)
	}
	if s.NetworkModeCounts["rdma"] != 1 {
		t.Errorf("network modes = %v", s.NetworkModeCounts)
	}
	if s.CPUModels["AMD EPYC 7763 64-Core Processor"] != 2 {
		t.Errorf("cpu models = %v", s.CPUModels)
	}
	if !slices.Equal(s.FailureDomains["rack-7"], []string{"srv-a", "srv-b"}) {
		t.Errorf("rack-7 = %v", s.FailureDomains["rack-7"])
	}
	if !slices.Equal(s.FailureDomains["unassigned"], []string{"srv-c"}) {
		t.Errorf("unassigned = %v", s.FailureDomains["unassigned"])
	}
}

func TestFindCapable(t *testing.T) {
	fleet := sampleFleet()

	if got := FindCapable(fleet, "local_nvme", "hugepages"); !slices.Equal(got, []string{"srv-a", "srv-b"}) {
		t.Errorf("nvme+hugepages = %v", got)
	}
	if got := FindCapable(fleet, "local_nvme", "rdma"); !slices.Equal(got, []string{"srv-a"}) {
		t.Errorf("nvme+rdma = %v", got)
	}
	if got := FindCapable(fleet, "quantum_link"); len(got) != 0 {
		t.Errorf("impossible requirement matched %v", got)
	}
	if got := FindCapable(fleet); len(got) != 3 {
		t.Errorf("empty requirement = %v", got)
	}
}

func TestFirmwareSpread(t *testing.T) {
	spread := FirmwareSpread(sampleFleet())

	nvme := spread["nvme:SAMSUNG MZQL21T9HCJR-00A07"]
	if nvme == nil {
		t.Fatalf("spread = %v", spread)
	}
	// srv-a has two drives on GDC7302Q but counts once; srv-b is on
	// GDC7304Q. The split is the finding.
	if nvme["GDC7302Q"] != 1 || nvme["GDC7304Q"] != 1 {
		t.Errorf("nvme spread = %v", nvme)
	}

	nic := spread["nic:mlx5_core"]
	if nic["22.36.1010"] != 1 {
		t.Errorf("nic spread = %v", nic)
	}
}

func TestFirmwareSpreadAfterStoreRoundTrip(t *testing.T) {
	// Facts that went through the store come back with Raw as generic
	// JSON maps; the spread must read them identically.
	var roundTripped []*facts.ServerFacts
	for _, f := range sampleFleet() {
		if f == nil {
			continue
		}
		data, err := json.Marshal(f)
		if err != nil {
			t.Fatal(err)
		}
		var back facts.ServerFacts
		if err := json.Unmarshal(data, &back); err != nil {
			t.Fatal(err)
		}
		roundTripped = append(roundTripped, &back)
	}

	direct := FirmwareSpread(sampleFleet())
	viaStore := FirmwareSpread(roundTripped)

	if len(viaStore) != len(direct) {
		t.Fatalf("spread size changed across round trip: %v vs %v", viaStore, direct)
	}
	for component, versions := range direct {
		for version, count := range versions {
			if viaStore[component][version] != count {
				t.Errorf("%s %s = %d after round trip, want %d",
					component, version, viaStore[component][version], count)
			}
		}
	}
}

func TestAggregateEmpty(t *testing.T) {
	s := Aggregate(nil)
	if s.Servers != 0 || len(s.FailureDomains) != 0 {
		t.Errorf("empty aggregate = %+v", s)
	}
}
