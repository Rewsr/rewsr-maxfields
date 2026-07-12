package collectors

import (
	"context"
	"testing"
)

// lldpctlSingleSwitch is lldpd's collapsed-container JSON for a host whose
// two fabric ports land on the same leaf switch.
const lldpctlSingleSwitch = `{
  "lldp": {
    "interface": [
      {
        "ens1f0np0": {
          "via": "LLDP",
          "rid": "1",
          "age": "0 day, 02:33:10",
          "chassis": {
            "leaf01": {
              "id": {"type": "mac", "value": "44:38:39:ff:00:01"},
              "descr": "Cumulus Linux version 5.9.2",
              "mgmt-ip": "10.0.0.11",
              "capability": [{"type": "Bridge", "enabled": true}, {"type": "Router", "enabled": true}]
            }
          },
          "port": {
            "id": {"type": "ifname", "value": "swp1"},
            "descr": "to-server-41-p0",
            "ttl": "120"
          }
        }
      },
      {
        "ens1f1np1": {
          "via": "LLDP",
          "rid": "1",
          "age": "0 day, 02:33:08",
          "chassis": {
            "leaf01": {
              "id": {"type": "mac", "value": "44:38:39:ff:00:01"},
              "descr": "Cumulus Linux version 5.9.2",
              "mgmt-ip": ["10.0.0.11", "fd00::11"]
            }
          },
          "port": {
            "id": {"type": "ifname", "value": "swp2"},
            "descr": "to-server-41-p1",
            "ttl": "120"
          }
        }
      }
    ]
  }
}`

// lldpctlDualHomed has two ports on two different leaves, so no single
// failure domain exists.
const lldpctlDualHomed = `{
  "lldp": {
    "interface": [
      {"ens1f0np0": {"chassis": {"leaf01": {"mgmt-ip": "10.0.0.11"}}, "port": {"id": {"type": "ifname", "value": "swp1"}}}},
      {"ens1f1np1": {"chassis": {"leaf02": {"mgmt-ip": "10.0.0.12"}}, "port": {"id": {"type": "ifname", "value": "swp1"}}}}
    ]
  }
}`

// lldpctlSinglePort is the fully collapsed form lldpd emits when exactly
// one interface has a neighbor: interface is an object, not an array.
const lldpctlSinglePort = `{"lldp": {"interface": {"eth0": {"chassis": {"tor-3": {"descr": "SONiC"}}, "port": {"id": {"type": "local", "value": "Ethernet12"}}}}}}`

func TestLLDPCollectorSingleSwitch(t *testing.T) {
	r := newFakeRunner()
	r.set("lldpctl -f json", lldpctlSingleSwitch)

	c := &LLDPCollector{Runner: r, LookPath: lookPathAll}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}

	neighbors, ok := f.Raw["lldp_neighbors"].([]lldpNeighbor)
	if !ok || len(neighbors) != 2 {
		t.Fatalf("Raw[lldp_neighbors] = %#v", f.Raw["lldp_neighbors"])
	}

	byPort := map[string]lldpNeighbor{}
	for _, n := range neighbors {
		byPort[n.LocalPort] = n
	}
	n0 := byPort["ens1f0np0"]
	if n0.ChassisName != "leaf01" || n0.PortID != "swp1" || n0.MgmtIP != "10.0.0.11" {
		t.Errorf("neighbor 0 = %+v", n0)
	}
	n1 := byPort["ens1f1np1"]
	if n1.MgmtIP != "10.0.0.11" {
		t.Errorf("list-form mgmt-ip not handled: %+v", n1)
	}

	if f.FailureDomain != "switch:leaf01" {
		t.Errorf("failure domain = %q, want switch:leaf01", f.FailureDomain)
	}
}

func TestLLDPCollectorDualHomedNoFailureDomain(t *testing.T) {
	r := newFakeRunner()
	r.set("lldpctl -f json", lldpctlDualHomed)

	c := &LLDPCollector{Runner: r, LookPath: lookPathAll}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}
	if f.FailureDomain != "" {
		t.Errorf("dual-homed host must not claim a single failure domain, got %q", f.FailureDomain)
	}
	if len(f.Raw["lldp_neighbors"].([]lldpNeighbor)) != 2 {
		t.Errorf("neighbors = %+v", f.Raw["lldp_neighbors"])
	}
}

func TestLLDPCollectorCollapsedSinglePort(t *testing.T) {
	r := newFakeRunner()
	r.set("lldpctl -f json", lldpctlSinglePort)

	c := &LLDPCollector{Runner: r, LookPath: lookPathAll}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}
	neighbors := f.Raw["lldp_neighbors"].([]lldpNeighbor)
	if len(neighbors) != 1 || neighbors[0].ChassisName != "tor-3" || neighbors[0].PortID != "Ethernet12" {
		t.Errorf("neighbors = %+v", neighbors)
	}
	if f.FailureDomain != "switch:tor-3" {
		t.Errorf("failure domain = %q", f.FailureDomain)
	}
}

func TestLLDPCollectorQuietFabric(t *testing.T) {
	r := newFakeRunner()
	r.set("lldpctl -f json", `{"lldp": {}}`)

	c := &LLDPCollector{Runner: r, LookPath: lookPathAll}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}
	if _, present := f.Raw["lldp_neighbors"]; present {
		t.Error("no neighbors should mean no Raw entry")
	}
}

func TestLLDPCollectorMissingTool(t *testing.T) {
	c := &LLDPCollector{Runner: newFakeRunner(), LookPath: lookPathNone}
	if _, err := c.Collect(context.Background(), "srv-1"); err == nil {
		t.Fatal("expected error without lldpctl")
	}
}
