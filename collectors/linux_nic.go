package collectors

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/Rewsr/rewsr-maxfields/facts"
)

// ipLinkEntry mirrors one element of `ip -j link` output.
type ipLinkEntry struct {
	IfName   string `json:"ifname"`
	Address  string `json:"address"`
	LinkType string `json:"link_type"`
}

func (c *LinuxHostCollector) collectNICs(ctx context.Context, lookPath func(string) (string, error), f *facts.ServerFacts) error {
	if err := requireCommand(lookPath, "ip"); err != nil {
		return err
	}

	out, err := c.Runner.Run(ctx, "ip", "-j", "link")
	if err != nil {
		return err
	}

	var links []ipLinkEntry
	if err := json.Unmarshal(out, &links); err != nil {
		return err
	}

	// ethtool is used for feature flags. It's optional: if it's missing
	// we still report the NICs we found via `ip link`, just without
	// Features populated.
	haveEthtool := requireCommand(lookPath, "ethtool") == nil

	var nics []facts.NICFacts
	for _, link := range links {
		if link.LinkType != "ether" {
			// Skip loopback, tunnels, and anything else that isn't a
			// real physical/virtual ethernet-style NIC.
			continue
		}

		nic := facts.NICFacts{
			Name:      link.IfName,
			MAC:       link.Address,
			SpeedGbps: readSpeedGbps(link.IfName),
			Driver:    readDriver(link.IfName),
		}

		if haveEthtool {
			if features, err := c.readEthtoolFeatures(ctx, link.IfName); err == nil {
				nic.Features = features
			}
		}

		if hasSRIOV(link.IfName) {
			nic.Features = append(nic.Features, "sriov")
		}

		nics = append(nics, nic)
	}
	f.NICs = nics

	return nil
}

// readEthtoolFeatures runs `ethtool -k <iface>` and returns the feature
// names currently reported "on". ethtool's -k output looks like:
//
//	Features for eth0:
//	rx-checksumming: on
//	tx-checksumming: on
//	tcp-segmentation-offload: on
//	generic-segmentation-offload: on [fixed]
func (c *LinuxHostCollector) readEthtoolFeatures(ctx context.Context, iface string) ([]string, error) {
	out, err := c.Runner.Run(ctx, "ethtool", "-k", iface)
	if err != nil {
		return nil, err
	}

	var features []string
	scanner := bufio.NewScanner(bytes.NewReader(out))
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" || strings.HasPrefix(line, "Features for") {
			continue
		}
		name, state, ok := strings.Cut(line, ":")
		if !ok {
			continue
		}
		state = strings.TrimSpace(state)
		// Some lines end with "[fixed]" for features that can't be
		// toggled; strip that before comparing the on/off state.
		if idx := strings.Index(state, "["); idx >= 0 {
			state = strings.TrimSpace(state[:idx])
		}
		if state == "on" {
			features = append(features, strings.TrimSpace(name))
		}
	}
	return features, nil
}

// readSpeedGbps reads /sys/class/net/<iface>/speed, which the kernel
// reports in Mbps. This is a direct sysfs read rather than a command,
// since it's just a file. Returns 0 if the file is missing or the link is
// down (the kernel reports -1 in that case).
func readSpeedGbps(iface string) int {
	data, err := os.ReadFile(filepath.Join("/sys/class/net", iface, "speed"))
	if err != nil {
		return 0
	}
	mbps, err := strconv.Atoi(strings.TrimSpace(string(data)))
	if err != nil || mbps <= 0 {
		return 0
	}
	return mbps / 1000
}

// readDriver resolves /sys/class/net/<iface>/device/driver, a symlink
// whose target's base name is the kernel driver name.
func readDriver(iface string) string {
	target, err := os.Readlink(filepath.Join("/sys/class/net", iface, "device", "driver"))
	if err != nil {
		return ""
	}
	return filepath.Base(target)
}

// hasSRIOV checks for a virtfn0 entry under the NIC's device directory,
// which only exists once at least one SR-IOV virtual function has been
// enabled on that physical function.
func hasSRIOV(iface string) bool {
	_, err := os.Stat(filepath.Join("/sys/class/net", iface, "device", "virtfn0"))
	return err == nil
}
