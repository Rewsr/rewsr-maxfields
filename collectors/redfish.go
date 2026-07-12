package collectors

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"time"

	"github.com/Rewsr/rewsr-facts/facts"
)

// RedfishCollector fetches power state and identity facts from a Redfish
// BMC over HTTP. It populates ServerFacts.Raw with vendor/model/serial
// style detail rather than typed fields, since those vary a lot by vendor
// and aren't part of the normalized ServerFacts contract.
type RedfishCollector struct {
	// BaseURL is the BMC's Redfish root, e.g. "https://10.0.1.5".
	BaseURL string
	// Token is sent as a bearer token if set.
	Token string

	HTTPClient *http.Client
}

// NewRedfishCollector returns a RedfishCollector with a bounded HTTP
// client. A short timeout matters here: a BMC on an unreachable management
// network should fail fast, not hang a whole collection cycle.
func NewRedfishCollector(baseURL, token string) *RedfishCollector {
	return &RedfishCollector{
		BaseURL:    baseURL,
		Token:      token,
		HTTPClient: &http.Client{Timeout: 5 * time.Second},
	}
}

// redfishSystem is the subset of the Redfish ComputerSystem schema we
// care about. See DMTF DSP0268 for the full schema.
type redfishSystem struct {
	PowerState   string `json:"PowerState"`
	Manufacturer string `json:"Manufacturer"`
	Model        string `json:"Model"`
	SerialNumber string `json:"SerialNumber"`
	Status       struct {
		Health string `json:"Health"`
		State  string `json:"State"`
	} `json:"Status"`
}

// Collect fetches GET {BaseURL}/redfish/v1/Systems/{serverID}. If the
// endpoint can't be reached or doesn't respond with a valid system (no lab
// BMC wired up in this build environment, for example), Collect falls back
// to a clearly-labeled demo response instead of failing outright, matching
// the demo-mode convention already used in rewsr-complete's TEE backends
// for attestation when no real hardware is available. The returned error
// still reports the underlying failure so callers can tell demo data from
// a real reading.
func (c *RedfishCollector) Collect(ctx context.Context, serverID string) (*facts.ServerFacts, error) {
	client := c.HTTPClient
	if client == nil {
		client = &http.Client{Timeout: 5 * time.Second}
	}

	url := strings.TrimRight(c.BaseURL, "/") + "/redfish/v1/Systems/" + serverID

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return demoRedfishFacts(serverID), fmt.Errorf("redfish: building request for %s: %w (using demo fallback)", url, err)
	}
	if c.Token != "" {
		req.Header.Set("Authorization", "Bearer "+c.Token)
	}
	req.Header.Set("Accept", "application/json")

	resp, err := client.Do(req)
	if err != nil {
		// No real Redfish endpoint reachable, e.g. running on a dev
		// machine with no BMC network access. Demo fallback below.
		return demoRedfishFacts(serverID), fmt.Errorf("redfish: %s unreachable: %w (using demo fallback)", url, err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return demoRedfishFacts(serverID), fmt.Errorf("redfish: %s returned status %d (using demo fallback)", url, resp.StatusCode)
	}

	var sys redfishSystem
	if err := json.NewDecoder(resp.Body).Decode(&sys); err != nil {
		return demoRedfishFacts(serverID), fmt.Errorf("redfish: decoding response from %s: %w (using demo fallback)", url, err)
	}

	return &facts.ServerFacts{
		ServerID:   serverID,
		Source:     "redfish",
		ObservedAt: time.Now().UTC(),
		PowerState: strings.ToLower(sys.PowerState),
		Raw: map[string]any{
			"vendor":         sys.Manufacturer,
			"model":          sys.Model,
			"serial_number":  sys.SerialNumber,
			"redfish_health": sys.Status.Health,
			"redfish_state":  sys.Status.State,
		},
	}, nil
}

// demoRedfishFacts is a canned response used only when no real Redfish
// endpoint is reachable. It is explicitly labeled as demo data in both
// the Source field and Raw, the same way rewsr-complete labels its canned
// Nitro attestation document ("DemoOnly") when no real enclave is present.
func demoRedfishFacts(serverID string) *facts.ServerFacts {
	return &facts.ServerFacts{
		ServerID:   serverID,
		Source:     "redfish_demo",
		ObservedAt: time.Now().UTC(),
		PowerState: "on",
		Raw: map[string]any{
			"vendor":        "Demo Vendor",
			"model":         "Demo Model X1",
			"serial_number": "DEMO-SERIAL-0000",
			"demo_mode":     true,
			"demo_reason":   "no reachable Redfish/BMC endpoint in this build environment",
		},
	}
}
