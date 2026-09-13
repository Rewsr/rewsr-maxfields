package collectors

import (
	"context"
	"fmt"
	"time"

	"github.com/Rewsr/rewsr-maxfields/facts"
	"github.com/Rewsr/rewsr-maxfields/store"
)

// ProvisioningCollector reads provisioning state from our own
// ProvisioningStore instead of probing a live system. Provisioning state
// and failure domain assignment are things our control plane already
// knows the moment it provisions a server; there's nothing to observe on
// the box itself, so this collector never touches the network or runs a
// command.
type ProvisioningCollector struct {
	Store store.ProvisioningStore
}

// NewProvisioningCollector returns a ProvisioningCollector reading from s.
func NewProvisioningCollector(s store.ProvisioningStore) *ProvisioningCollector {
	return &ProvisioningCollector{Store: s}
}

// Collect looks up serverID's provisioning record. If there is no record
// on file (server not provisioned through us, or not provisioned yet),
// Collect returns an empty ServerFacts and a non-nil error rather than
// guessing.
func (c *ProvisioningCollector) Collect(ctx context.Context, serverID string) (*facts.ServerFacts, error) {
	if c.Store == nil {
		return &facts.ServerFacts{ServerID: serverID, Source: "provisioning_store"}, fmt.Errorf("provisioning collector: no ProvisioningStore configured")
	}

	provisioning, failureDomain, err := c.Store.GetProvisioning(ctx, serverID)
	if err != nil {
		return &facts.ServerFacts{ServerID: serverID, Source: "provisioning_store"}, fmt.Errorf("provisioning collector: %w", err)
	}

	return &facts.ServerFacts{
		ServerID:      serverID,
		Source:        "provisioning_store",
		ObservedAt:    time.Now().UTC(),
		Provisioning:  provisioning,
		FailureDomain: failureDomain,
	}, nil
}
