package facts

import (
	"context"
	"errors"
	"testing"
)

type fakeCollector struct {
	facts *ServerFacts
	err   error
}

func (f *fakeCollector) Collect(ctx context.Context, serverID string) (*ServerFacts, error) {
	return f.facts, f.err
}

func TestBuildServerFacts_MergesGapsNotOverwrites(t *testing.T) {
	first := &fakeCollector{facts: &ServerFacts{
		ServerID:   "srv-1",
		Source:     "linux_host",
		PowerState: "on",
		CPU:        CPUFacts{Vendor: "GenuineIntel", Cores: 8},
		MemoryGB:   64,
	}}
	// second collector reports a different PowerState; it should NOT
	// overwrite the value already set by the first collector.
	second := &fakeCollector{facts: &ServerFacts{
		ServerID:      "srv-1",
		Source:        "redfish",
		PowerState:    "off",
		Provisioning:  "provisioned",
		FailureDomain: "fd-1",
	}}

	got, err := BuildServerFacts(context.Background(), "srv-1", []Collector{first, second})
	if err != nil {
		t.Fatalf("BuildServerFacts returned error: %v", err)
	}

	if got.PowerState != "on" {
		t.Errorf("PowerState = %q, want %q (first collector's value should win)", got.PowerState, "on")
	}
	if got.Provisioning != "provisioned" {
		t.Errorf("Provisioning = %q, want %q (second collector should fill the gap)", got.Provisioning, "provisioned")
	}
	if got.FailureDomain != "fd-1" {
		t.Errorf("FailureDomain = %q, want %q", got.FailureDomain, "fd-1")
	}
	if got.CPU.Cores != 8 {
		t.Errorf("CPU.Cores = %d, want 8", got.CPU.Cores)
	}
	if got.MemoryGB != 64 {
		t.Errorf("MemoryGB = %d, want 64", got.MemoryGB)
	}
	if got.Source != "linux_host+redfish" {
		t.Errorf("Source = %q, want %q", got.Source, "linux_host+redfish")
	}
}

func TestBuildServerFacts_AllCollectorsFail(t *testing.T) {
	c1 := &fakeCollector{err: errors.New("boom")}
	c2 := &fakeCollector{err: errors.New("also boom")}

	got, err := BuildServerFacts(context.Background(), "srv-1", []Collector{c1, c2})
	if err == nil {
		t.Fatal("expected an error when all collectors fail")
	}
	if got != nil {
		t.Errorf("expected nil facts when all collectors fail, got %+v", got)
	}
}

func TestBuildServerFacts_PartialFailureStillMerges(t *testing.T) {
	ok := &fakeCollector{facts: &ServerFacts{ServerID: "srv-1", Source: "linux_host", MemoryGB: 32}}
	bad := &fakeCollector{err: errors.New("no redfish endpoint")}

	got, err := BuildServerFacts(context.Background(), "srv-1", []Collector{ok, bad})
	if err != nil {
		t.Fatalf("expected no error with at least one success, got %v", err)
	}
	if got.MemoryGB != 32 {
		t.Errorf("MemoryGB = %d, want 32", got.MemoryGB)
	}
}

func TestBuildServerFacts_InfersCapabilitiesAndHealth(t *testing.T) {
	c := &fakeCollector{facts: &ServerFacts{
		ServerID:   "srv-1",
		Source:     "linux_host",
		PowerState: "on",
		CPU:        CPUFacts{Cores: 4},
		MemoryGB:   16,
		Disks:      []DiskFacts{{Name: "nvme0n1", Type: "nvme", SizeGB: 500}},
		NICs: []NICFacts{
			{Name: "eth0", SpeedGbps: 25, Features: []string{"sriov"}},
		},
	}}

	got, err := BuildServerFacts(context.Background(), "srv-1", []Collector{c})
	if err != nil {
		t.Fatalf("BuildServerFacts returned error: %v", err)
	}

	wantCaps := map[string]bool{"local_nvme": true, "high_bandwidth_networking": true, "sriov": true, "power_control": true}
	for cap := range wantCaps {
		if !hasString(got.Capabilities, cap) {
			t.Errorf("Capabilities = %v, missing %q", got.Capabilities, cap)
		}
	}

	if got.Health != "healthy" {
		t.Errorf("Health = %q, want %q", got.Health, "healthy")
	}
}

func hasString(list []string, want string) bool {
	for _, s := range list {
		if s == want {
			return true
		}
	}
	return false
}
