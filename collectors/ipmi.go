package collectors

import (
	"context"
	"errors"
	"os/exec"
	"strconv"
	"strings"
	"time"

	"github.com/Rewsr/rewsr-facts/facts"
	"github.com/Rewsr/rewsr-facts/runner"
)

// IPMICollector reads chassis power state and sensor data through the
// host's own BMC interface via ipmitool. It covers the fleet segment whose
// BMCs predate usable Redfish: same facts, older transport. Like the
// Redfish collector it can report power state; the merge keeps whichever
// collector answered first, so running both is safe.
type IPMICollector struct {
	Runner runner.CommandRunner

	// LookPath resolves a command name to a path, or errors if the
	// command isn't installed. Defaults to exec.LookPath; overridable in
	// tests.
	LookPath func(file string) (string, error)
}

// NewIPMICollector returns an IPMICollector running commands through r.
func NewIPMICollector(r runner.CommandRunner) *IPMICollector {
	return &IPMICollector{Runner: r, LookPath: exec.LookPath}
}

// bmcSensors is the summary carried in Raw["bmc_sensors"].
type bmcSensors struct {
	TempsC      map[string]float64 `json:"temps_c,omitempty"`
	FansRPM     map[string]int     `json:"fans_rpm,omitempty"`
	PowerWatts  float64            `json:"power_watts,omitempty"`
	SensorCount int                `json:"sensor_count"`
	NotOK       []string           `json:"not_ok,omitempty"`
}

// Collect gathers power state and sensors. The sensor listing failing
// while power status works (or vice versa) still yields partial facts and
// a nil error; only ipmitool being absent or both commands failing is an
// error.
func (c *IPMICollector) Collect(ctx context.Context, serverID string) (*facts.ServerFacts, error) {
	f := &facts.ServerFacts{
		ServerID:   serverID,
		Source:     "ipmi",
		ObservedAt: time.Now().UTC(),
		Raw:        map[string]any{},
	}

	if c.Runner == nil {
		return f, errors.New("ipmi collector: no CommandRunner configured")
	}
	lookPath := c.LookPath
	if lookPath == nil {
		lookPath = exec.LookPath
	}
	if err := requireCommand(lookPath, "ipmitool"); err != nil {
		return f, errors.New("ipmi collector: " + err.Error())
	}

	okSources := 0

	if out, err := c.Runner.Run(ctx, "ipmitool", "chassis", "power", "status"); err == nil {
		if state := parseChassisPowerStatus(out); state != "" {
			f.PowerState = state
			okSources++
		}
	}

	if out, err := c.Runner.Run(ctx, "ipmitool", "sdr", "elist"); err == nil {
		sensors := parseSdrElist(out)
		if sensors.SensorCount > 0 {
			f.Raw["bmc_sensors"] = sensors
			okSources++
		}
	}

	if okSources == 0 {
		return f, errors.New("ipmi collector: ipmitool present but no usable output (no local BMC?)")
	}
	return f, nil
}

// parseChassisPowerStatus parses "Chassis Power is on" into the same
// lowercase vocabulary the Redfish collector uses.
func parseChassisPowerStatus(out []byte) string {
	s := strings.TrimSpace(string(out))
	const prefix = "Chassis Power is "
	if !strings.HasPrefix(s, prefix) {
		return ""
	}
	return strings.ToLower(strings.TrimSpace(strings.TrimPrefix(s, prefix)))
}

// parseSdrElist parses `ipmitool sdr elist` lines, which look like:
//
//	Inlet Temp       | 04h | ok  |  7.1 | 24 degrees C
//	Fan1A            | 30h | ok  |  7.1 | 9240 RPM
//	Pwr Consumption  | 77h | ok  |  7.1 | 288 Watts
//	PSU2 Status      | 75h | cr  | 10.2 | Failure detected
//
// Temperatures, fan speeds, and the power reading get typed fields; any
// sensor whose status column is not "ok" or "ns" (no reading) is listed in
// NotOK by name, which is the piece an operator actually pages on.
func parseSdrElist(out []byte) bmcSensors {
	sensors := bmcSensors{
		TempsC:  map[string]float64{},
		FansRPM: map[string]int{},
	}

	for _, line := range strings.Split(string(out), "\n") {
		parts := strings.Split(line, "|")
		if len(parts) < 5 {
			continue
		}
		name := strings.TrimSpace(parts[0])
		status := strings.TrimSpace(parts[2])
		reading := strings.TrimSpace(parts[4])
		if name == "" {
			continue
		}
		sensors.SensorCount++

		if status != "ok" && status != "ns" {
			sensors.NotOK = append(sensors.NotOK, name+": "+reading)
		}

		switch {
		case strings.HasSuffix(reading, "degrees C"):
			v := strings.TrimSpace(strings.TrimSuffix(reading, "degrees C"))
			sensors.TempsC[name] = parseFloatOrZero(v)
		case strings.HasSuffix(reading, "RPM"):
			v := strings.TrimSpace(strings.TrimSuffix(reading, "RPM"))
			sensors.FansRPM[name] = atoiOrZero(v)
		case strings.HasSuffix(reading, "Watts"):
			v := strings.TrimSpace(strings.TrimSuffix(reading, "Watts"))
			// Multiple watt sensors exist on some platforms; keep the
			// largest, which is the whole-chassis consumption.
			if w := parseFloatOrZero(v); w > sensors.PowerWatts {
				sensors.PowerWatts = w
			}
		}
	}

	if len(sensors.TempsC) == 0 {
		sensors.TempsC = nil
	}
	if len(sensors.FansRPM) == 0 {
		sensors.FansRPM = nil
	}
	return sensors
}

// parseFloatOrZero parses a decimal reading like "24" or "24.5", returning
// 0 on anything unparseable, matching atoiOrZero's contract.
func parseFloatOrZero(s string) float64 {
	v, err := strconv.ParseFloat(strings.TrimSpace(s), 64)
	if err != nil {
		return 0
	}
	return v
}
