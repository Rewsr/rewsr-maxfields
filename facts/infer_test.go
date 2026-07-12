package facts

import "testing"

func TestInferHealth(t *testing.T) {
	tests := []struct {
		name string
		f    *ServerFacts
		want string
	}{
		{"nil facts", nil, "unknown"},
		{"nothing collected", &ServerFacts{}, "unknown"},
		{"powered off", &ServerFacts{PowerState: "off", CPU: CPUFacts{Cores: 4}}, "degraded"},
		{"provisioning failed", &ServerFacts{Provisioning: "failed", PowerState: "on", CPU: CPUFacts{Cores: 4}}, "degraded"},
		{"power on but no hardware facts", &ServerFacts{PowerState: "on"}, "degraded"},
		{"power on with hardware facts", &ServerFacts{PowerState: "on", CPU: CPUFacts{Cores: 4}, MemoryGB: 16}, "healthy"},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := InferHealth(tt.f); got != tt.want {
				t.Errorf("InferHealth() = %q, want %q", got, tt.want)
			}
		})
	}
}

func TestInferCapabilities(t *testing.T) {
	f := &ServerFacts{
		PowerState: "on",
		Disks:      []DiskFacts{{Type: "hdd"}, {Type: "nvme"}},
		NICs: []NICFacts{
			{SpeedGbps: 10},
			{SpeedGbps: 100, Features: []string{"rdma", "sriov"}},
		},
	}

	got := InferCapabilities(f)
	want := []string{"high_bandwidth_networking", "local_nvme", "power_control", "rdma", "sriov"}

	if len(got) != len(want) {
		t.Fatalf("InferCapabilities() = %v, want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("InferCapabilities()[%d] = %q, want %q (full: %v)", i, got[i], want[i], got)
		}
	}
}

func TestInferCapabilities_NoSignals(t *testing.T) {
	f := &ServerFacts{}
	got := InferCapabilities(f)
	if len(got) != 0 {
		t.Errorf("InferCapabilities() = %v, want empty", got)
	}
}
