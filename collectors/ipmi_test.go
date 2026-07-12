package collectors

import (
	"context"
	"slices"
	"testing"
)

const sdrElistOutput = `Inlet Temp       | 04h | ok  |  7.1 | 24 degrees C
Exhaust Temp     | 05h | ok  |  7.1 | 38 degrees C
CPU1 Temp        | 01h | ok  |  3.1 | 45 degrees C
CPU2 Temp        | 02h | ok  |  3.2 | 47 degrees C
Fan1A            | 30h | ok  |  7.1 | 9240 RPM
Fan1B            | 31h | ok  |  7.1 | 8280 RPM
Fan2A            | 32h | ok  |  7.1 | 9360 RPM
PSU1 Status      | 74h | ok  | 10.1 | Presence detected
PSU2 Status      | 75h | cr  | 10.2 | Failure detected
Pwr Consumption  | 77h | ok  |  7.1 | 288 Watts
PSU1 PG Fail     | 78h | ns  | 10.1 | Disabled
Voltage 1        | 6Ah | ok  |  7.1 | 224 Volts
`

func TestIPMICollector(t *testing.T) {
	r := newFakeRunner()
	r.set("ipmitool chassis power status", "Chassis Power is on\n")
	r.set("ipmitool sdr elist", sdrElistOutput)

	c := &IPMICollector{Runner: r, LookPath: lookPathAll}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}

	if f.PowerState != "on" {
		t.Errorf("power state = %q", f.PowerState)
	}

	sensors, ok := f.Raw["bmc_sensors"].(bmcSensors)
	if !ok {
		t.Fatalf("Raw[bmc_sensors] = %#v", f.Raw["bmc_sensors"])
	}
	if sensors.SensorCount != 12 {
		t.Errorf("sensor count = %d, want 12", sensors.SensorCount)
	}
	if sensors.TempsC["Inlet Temp"] != 24 || sensors.TempsC["CPU2 Temp"] != 47 {
		t.Errorf("temps = %v", sensors.TempsC)
	}
	if sensors.FansRPM["Fan1A"] != 9240 {
		t.Errorf("fans = %v", sensors.FansRPM)
	}
	if sensors.PowerWatts != 288 {
		t.Errorf("power = %v", sensors.PowerWatts)
	}
	if len(sensors.NotOK) != 1 || !slices.Contains(sensors.NotOK, "PSU2 Status: Failure detected") {
		t.Errorf("not-ok sensors = %v", sensors.NotOK)
	}
}

func TestIPMICollectorPowerOnly(t *testing.T) {
	r := newFakeRunner()
	r.set("ipmitool chassis power status", "Chassis Power is off\n")
	r.setErr("ipmitool sdr elist", errContext("no SDR repository"))

	c := &IPMICollector{Runner: r, LookPath: lookPathAll}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}
	if f.PowerState != "off" {
		t.Errorf("power state = %q", f.PowerState)
	}
	if _, present := f.Raw["bmc_sensors"]; present {
		t.Error("sensors should be absent")
	}
}

func TestIPMICollectorNoBMC(t *testing.T) {
	r := newFakeRunner()
	r.setErr("ipmitool chassis power status", errContext("Could not open device at /dev/ipmi0"))
	r.setErr("ipmitool sdr elist", errContext("Could not open device at /dev/ipmi0"))

	c := &IPMICollector{Runner: r, LookPath: lookPathAll}
	if _, err := c.Collect(context.Background(), "srv-1"); err == nil {
		t.Fatal("expected error when the BMC is unreachable")
	}
}

func TestIPMICollectorMissingTool(t *testing.T) {
	c := &IPMICollector{Runner: newFakeRunner(), LookPath: lookPathNone}
	if _, err := c.Collect(context.Background(), "srv-1"); err == nil {
		t.Fatal("expected error without ipmitool")
	}
}

func TestParseChassisPowerStatus(t *testing.T) {
	cases := map[string]string{
		"Chassis Power is on\n":  "on",
		"Chassis Power is off\n": "off",
		"garbage":                "",
	}
	for in, want := range cases {
		if got := parseChassisPowerStatus([]byte(in)); got != want {
			t.Errorf("parseChassisPowerStatus(%q) = %q, want %q", in, got, want)
		}
	}
}
