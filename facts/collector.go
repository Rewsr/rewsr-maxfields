package facts

import "context"

// Collector gathers facts about one server from a single source (a Linux
// host itself, a Redfish BMC, an internal provisioning store, and so on).
// A Collector should return partial facts and a non-nil error rather than
// panicking when its source is unavailable; BuildServerFacts decides how to
// handle that.
type Collector interface {
	Collect(ctx context.Context, serverID string) (*ServerFacts, error)
}
