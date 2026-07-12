package collectors

import (
	"context"
	"slices"
	"testing"
)

const ethtoolDriverInfo = `driver: mlx5_core
version: 6.8.0-41-generic
firmware-version: 22.36.1010 (MT_0000000437)
expansion-rom-version:
bus-info: 0000:41:00.0
supports-statistics: yes
supports-test: yes
supports-eeprom-access: no
supports-register-dump: no
supports-priv-flags: yes
`

const ethtoolRings = `Ring parameters for ens1f0np0:
Pre-set maximums:
RX:		8192
RX Mini:	n/a
RX Jumbo:	n/a
TX:		8192
Current hardware settings:
RX:		1024
RX Mini:	n/a
RX Jumbo:	n/a
TX:		1024
`

const ethtoolChannels = `Channel parameters for ens1f0np0:
Pre-set maximums:
RX:		n/a
TX:		n/a
Other:		n/a
Combined:	63
Current hardware settings:
RX:		n/a
TX:		n/a
Other:		n/a
Combined:	16
`

const ethtoolTimestamping = `Time stamping parameters for ens1f0np0:
Capabilities:
	hardware-transmit
	software-transmit
	hardware-receive
	software-receive
	software-system-clock
	hardware-raw-clock
PTP Hardware Clock: 0
Hardware Transmit Timestamp Modes:
	off
	on
Hardware Receive Filter Modes:
	none
	all
`

const ethtoolTimestampingNone = `Time stamping parameters for eth1:
Capabilities:
	software-transmit
	software-receive
	software-system-clock
PTP Hardware Clock: none
Hardware Transmit Timestamp Modes: none
Hardware Receive Filter Modes: none
`

func TestNICOffloadCollector(t *testing.T) {
	r := newFakeRunner()
	r.set("ip -j link", `[{"ifname":"lo","link_type":"loopback","address":"00:00:00:00:00:00"},{"ifname":"ens1f0np0","link_type":"ether","address":"b8:59:9f:ca:e2:f8"},{"ifname":"eth1","link_type":"ether","address":"b8:59:9f:ca:e2:f9"}]`)
	r.set("ethtool -i ens1f0np0", ethtoolDriverInfo)
	r.set("ethtool -g ens1f0np0", ethtoolRings)
	r.set("ethtool -l ens1f0np0", ethtoolChannels)
	r.set("ethtool -T ens1f0np0", ethtoolTimestamping)
	r.set("ethtool -i eth1", "driver: virtio_net\nversion: 1.0.0\nbus-info: 0000:00:03.0\n")
	r.setErr("ethtool -g eth1", errContext("virtio rejects ring query"))
	r.setErr("ethtool -l eth1", errContext("virtio rejects channel query"))
	r.set("ethtool -T eth1", ethtoolTimestampingNone)

	c := &NICOffloadCollector{Runner: r, LookPath: lookPathAll}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}

	perNIC, ok := f.Raw["nic_offload"].(map[string]nicOffloadFacts)
	if !ok {
		t.Fatalf("Raw[nic_offload] = %#v", f.Raw["nic_offload"])
	}
	if len(perNIC) != 2 {
		t.Fatalf("want 2 NICs, got %v", perNIC)
	}

	mlx := perNIC["ens1f0np0"]
	if mlx.Driver["driver"] != "mlx5_core" || mlx.Driver["firmware-version"] != "22.36.1010 (MT_0000000437)" {
		t.Errorf("driver info = %v", mlx.Driver)
	}
	if mlx.RingsMax["rx"] != 8192 || mlx.RingsCurrent["rx"] != 1024 {
		t.Errorf("rings = max %v current %v", mlx.RingsMax, mlx.RingsCurrent)
	}
	if mlx.ChannelsMax["combined"] != 63 || mlx.ChannelsCurrent["combined"] != 16 {
		t.Errorf("channels = max %v current %v", mlx.ChannelsMax, mlx.ChannelsCurrent)
	}
	if mlx.PHCIndex != 0 {
		t.Errorf("phc index = %d, want 0", mlx.PHCIndex)
	}
	if !slices.Contains(mlx.TimestampCaps, "hardware-raw-clock") {
		t.Errorf("timestamp caps = %v", mlx.TimestampCaps)
	}

	virtio := perNIC["eth1"]
	if virtio.PHCIndex != -1 {
		t.Errorf("virtio phc index = %d, want -1", virtio.PHCIndex)
	}
	if virtio.RingsMax != nil {
		t.Errorf("failed ring query must leave section empty: %v", virtio.RingsMax)
	}

	if !slices.Contains(f.Capabilities, "ptp_hardware_clock") {
		t.Errorf("capabilities = %v", f.Capabilities)
	}
}

func TestNICOffloadCollectorNoEthtool(t *testing.T) {
	lookPath := func(name string) (string, error) {
		if name == "ethtool" {
			return lookPathNone(name)
		}
		return lookPathAll(name)
	}
	c := &NICOffloadCollector{Runner: newFakeRunner(), LookPath: lookPath}
	if _, err := c.Collect(context.Background(), "srv-1"); err == nil {
		t.Fatal("expected error without ethtool")
	}
}

func TestParseEthtoolMaxCurrentAllNA(t *testing.T) {
	max, current := parseEthtoolMaxCurrent([]byte(`Channel parameters for eth0:
Pre-set maximums:
RX:		n/a
TX:		n/a
Current hardware settings:
RX:		n/a
TX:		n/a
`))
	if max != nil || current != nil {
		t.Errorf("all-n/a should produce nil maps, got %v %v", max, current)
	}
}

// errContext makes a distinct error for scripted command failures.
func errContext(msg string) error {
	return &scriptedError{msg}
}

type scriptedError struct{ msg string }

func (e *scriptedError) Error() string { return e.msg }
