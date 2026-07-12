package collectors

import (
	"context"
	"slices"
	"testing"
)

const rdmaLinkShowJSON = `[{"ifindex":0,"ifname":"mlx5_0","port":1,"state":"ACTIVE","physical_state":"LINK_UP","netdev":"ens1f0np0"},{"ifindex":1,"ifname":"mlx5_1","port":1,"state":"DOWN","physical_state":"DISABLED","netdev":"ens1f1np1"}]`

const ibvDevinfoOutput = `hca_id:	mlx5_0
	transport:			InfiniBand (0)
	fw_ver:				20.39.2048
	node_guid:			b859:9f03:00ca:e2f8
	sys_image_guid:			b859:9f03:00ca:e2f8
	vendor_id:			0x02c9
	vendor_part_id:			4123
	hw_ver:				0x0
	board_id:			MT_0000000223
	phys_port_cnt:			1
		port:	1
			state:			PORT_ACTIVE (4)
			max_mtu:		4096 (5)
			active_mtu:		4096 (5)
			sm_lid:			1
			port_lid:		4
			port_lmc:		0x00
			link_layer:		InfiniBand

hca_id:	mlx5_1
	transport:			InfiniBand (0)
	fw_ver:				22.36.1010
	node_guid:			b859:9f03:00ca:e2f9
	sys_image_guid:			b859:9f03:00ca:e2f9
	vendor_id:			0x02c9
	vendor_part_id:			4125
	hw_ver:				0x0
	board_id:			MT_0000000437
	phys_port_cnt:			1
		port:	1
			state:			PORT_DOWN (1)
			max_mtu:		4096 (5)
			active_mtu:		1024 (3)
			sm_lid:			0
			port_lid:		65535
			port_lmc:		0x00
			link_layer:		Ethernet
`

func TestRDMACollectorViaRdmaTool(t *testing.T) {
	root := t.TempDir()
	writeTree(t, root, map[string]string{
		"sys/class/infiniband/mlx5_0/ports/1/link_layer": "Ethernet\n",
		"sys/class/infiniband/mlx5_0/fw_ver":             "22.36.1010\n",
		"sys/class/infiniband/mlx5_1/ports/1/link_layer": "Ethernet\n",
	})

	r := newFakeRunner()
	r.set("rdma link show -j", rdmaLinkShowJSON)

	c := &RDMACollector{Runner: r, LookPath: lookPathAll, SysRoot: root}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}

	for _, want := range []string{"rdma", "roce", "rdma_active"} {
		if !slices.Contains(f.Capabilities, want) {
			t.Errorf("capabilities missing %q: %v", want, f.Capabilities)
		}
	}
	if slices.Contains(f.Capabilities, "infiniband") {
		t.Errorf("infiniband should not be reported for Ethernet link layer: %v", f.Capabilities)
	}
	if !slices.Contains(f.NetworkModes, "rdma") {
		t.Errorf("network modes missing rdma: %v", f.NetworkModes)
	}

	devices, ok := f.Raw["rdma"].([]rdmaDevice)
	if !ok || len(devices) != 2 {
		t.Fatalf("Raw[rdma] = %#v", f.Raw["rdma"])
	}
	if devices[0].Netdev != "ens1f0np0" || devices[0].FirmwareVer != "22.36.1010" {
		t.Errorf("device 0 = %+v", devices[0])
	}
}

func TestRDMACollectorFallsBackToIbvDevinfo(t *testing.T) {
	r := newFakeRunner()
	r.set("ibv_devinfo", ibvDevinfoOutput)

	lookPath := func(name string) (string, error) {
		if name == "rdma" {
			return lookPathNone(name)
		}
		return lookPathAll(name)
	}

	c := &RDMACollector{Runner: r, LookPath: lookPath, SysRoot: t.TempDir()}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}

	devices := f.Raw["rdma"].([]rdmaDevice)
	if len(devices) != 2 {
		t.Fatalf("want 2 devices, got %+v", devices)
	}
	if devices[0].Device != "mlx5_0" || devices[0].State != "ACTIVE" || devices[0].LinkLayer != "InfiniBand" {
		t.Errorf("device 0 = %+v", devices[0])
	}
	if devices[0].Transport != "InfiniBand" || devices[0].FirmwareVer != "20.39.2048" {
		t.Errorf("device 0 transport/fw = %+v", devices[0])
	}
	if devices[1].State != "DOWN" || devices[1].LinkLayer != "Ethernet" {
		t.Errorf("device 1 = %+v", devices[1])
	}

	if !slices.Contains(f.Capabilities, "infiniband") || !slices.Contains(f.Capabilities, "roce") {
		t.Errorf("capabilities = %v", f.Capabilities)
	}
}

func TestRDMACollectorEmptySubsystemIsNotAnError(t *testing.T) {
	r := newFakeRunner()
	r.set("rdma link show -j", "[]")

	c := &RDMACollector{Runner: r, LookPath: lookPathAll, SysRoot: t.TempDir()}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}
	if len(f.Capabilities) != 0 || len(f.NetworkModes) != 0 {
		t.Errorf("no capabilities expected: %v %v", f.Capabilities, f.NetworkModes)
	}
	if _, present := f.Raw["rdma"]; present {
		t.Error("Raw[rdma] should be absent with zero devices")
	}
}

func TestRDMACollectorNoToolsAtAll(t *testing.T) {
	c := &RDMACollector{Runner: newFakeRunner(), LookPath: lookPathNone, SysRoot: t.TempDir()}
	f, err := c.Collect(context.Background(), "srv-1")
	if err == nil {
		t.Fatal("expected error when neither rdma nor ibv_devinfo exists")
	}
	if f == nil {
		t.Fatal("facts must be non-nil even on error")
	}
}

func TestNormalizeIbvPortState(t *testing.T) {
	cases := map[string]string{
		"PORT_ACTIVE": "ACTIVE",
		"PORT_DOWN":   "DOWN",
		"PORT_INIT":   "INIT",
		"ACTIVE":      "ACTIVE",
	}
	for in, want := range cases {
		if got := normalizeIbvPortState(in); got != want {
			t.Errorf("normalizeIbvPortState(%q) = %q, want %q", in, got, want)
		}
	}
}
