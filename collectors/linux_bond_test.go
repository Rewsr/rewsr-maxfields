package collectors

import (
	"context"
	"slices"
	"testing"
)

const procNetBonding = `Ethernet Channel Bonding Driver: v6.8.0-41-generic

Bonding Mode: IEEE 802.3ad Dynamic link aggregation
Transmit Hash Policy: layer3+4 (1)
MII Status: up
MII Polling Interval (ms): 100
Up Delay (ms): 0
Down Delay (ms): 0
Peer Notification Delay (ms): 0

802.3ad info
LACP active: on
LACP rate: fast
Min links: 0
Aggregator selection policy (ad_select): stable
System priority: 65535
System MAC address: b8:59:9f:ca:e2:f8
Active Aggregator Info:
	Aggregator ID: 1
	Number of ports: 2
	Actor Key: 15
	Partner Key: 201
	Partner Mac Address: 44:38:39:ff:00:01

Slave Interface: ens1f0np0
MII Status: up
Speed: 25000 Mbps
Duplex: full
Link Failure Count: 0
Permanent HW addr: b8:59:9f:ca:e2:f8
Slave queue ID: 0
Aggregator ID: 1

Slave Interface: ens1f1np1
MII Status: down
Speed: 25000 Mbps
Duplex: full
Link Failure Count: 2
Permanent HW addr: b8:59:9f:ca:e2:f9
Slave queue ID: 0
Aggregator ID: 1
`

func TestNetTopologyCollectorLACPBond(t *testing.T) {
	root := t.TempDir()
	writeTree(t, root, map[string]string{
		"proc/net/bonding/bond0": procNetBonding,
	})

	r := newFakeRunner()
	r.set("ip -j -d link", `[
	  {"ifname":"lo","link_type":"loopback"},
	  {"ifname":"ens1f0np0","link_type":"ether"},
	  {"ifname":"ens1f1np1","link_type":"ether"},
	  {"ifname":"bond0","link_type":"ether","linkinfo":{"info_kind":"bond","info_data":{"mode":"802.3ad"}}},
	  {"ifname":"bond0.120","link_type":"ether","linkinfo":{"info_kind":"vlan","info_data":{"protocol":"802.1Q","id":120}}},
	  {"ifname":"br0","link_type":"ether","linkinfo":{"info_kind":"bridge"}}
	]`)

	c := &NetTopologyCollector{Runner: r, LookPath: lookPathAll, SysRoot: root}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}

	for _, want := range []string{"bonded", "lacp", "vlan_tagged", "bridged"} {
		if !slices.Contains(f.NetworkModes, want) {
			t.Errorf("network modes missing %q: %v", want, f.NetworkModes)
		}
	}

	topo := f.Raw["net_topology"].(map[string]any)
	bonds := topo["bonds"].([]bondFacts)
	if len(bonds) != 1 {
		t.Fatalf("bonds = %+v", bonds)
	}
	b := bonds[0]
	if b.Name != "bond0" || !slices.Contains([]string{"IEEE 802.3ad Dynamic link aggregation"}, b.Mode) {
		t.Errorf("bond = %+v", b)
	}
	if b.HashPolicy != "layer3+4 (1)" || b.LACPRate != "fast" || b.LACPActive != "on" {
		t.Errorf("bond lacp fields = %+v", b)
	}
	if len(b.Slaves) != 2 {
		t.Fatalf("slaves = %+v", b.Slaves)
	}
	if b.Slaves[0].Name != "ens1f0np0" || b.Slaves[0].MIIStatus != "up" || b.Slaves[0].SpeedMbps != 25000 {
		t.Errorf("slave 0 = %+v", b.Slaves[0])
	}
	if b.Slaves[1].MIIStatus != "down" {
		t.Errorf("slave 1 = %+v", b.Slaves[1])
	}

	if vlans := topo["vlans"].([]string); !slices.Contains(vlans, "bond0.120") {
		t.Errorf("vlans = %v", vlans)
	}
}

func TestNetTopologyCollectorPlainHost(t *testing.T) {
	r := newFakeRunner()
	r.set("ip -j -d link", `[{"ifname":"lo","link_type":"loopback"},{"ifname":"eth0","link_type":"ether"}]`)

	c := &NetTopologyCollector{Runner: r, LookPath: lookPathAll, SysRoot: t.TempDir()}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}
	if len(f.NetworkModes) != 0 {
		t.Errorf("plain host must report no modes: %v", f.NetworkModes)
	}
	if _, present := f.Raw["net_topology"]; present {
		t.Error("Raw[net_topology] should be absent on a plain host")
	}
}

func TestNetTopologyCollectorBondWithoutProcFile(t *testing.T) {
	// The bond exists in ip link but /proc/net/bonding is unreadable;
	// the bond is still reported, just without detail.
	r := newFakeRunner()
	r.set("ip -j -d link", `[{"ifname":"bond0","link_type":"ether","linkinfo":{"info_kind":"bond"}}]`)

	c := &NetTopologyCollector{Runner: r, LookPath: lookPathAll, SysRoot: t.TempDir()}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}
	if !slices.Contains(f.NetworkModes, "bonded") {
		t.Errorf("modes = %v", f.NetworkModes)
	}
	if slices.Contains(f.NetworkModes, "lacp") {
		t.Errorf("lacp must not be claimed without the proc file: %v", f.NetworkModes)
	}
}

func TestNetTopologyCollectorNoIP(t *testing.T) {
	c := &NetTopologyCollector{Runner: newFakeRunner(), LookPath: lookPathNone}
	if _, err := c.Collect(context.Background(), "srv-1"); err == nil {
		t.Fatal("expected error without ip")
	}
}
