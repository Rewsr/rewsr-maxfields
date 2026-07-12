// Package refresh runs facts collection on a schedule and writes results to
// a FactsStore, so the API layer only ever reads cached facts and never
// blocks a request on live hardware collection.
package refresh

import (
	"context"
	"log"
	"time"

	"github.com/Rewsr/rewsr-facts/facts"
	"github.com/Rewsr/rewsr-facts/store"
)

// Refresher periodically runs BuildServerFacts for a fixed set of servers
// and saves the result to a FactsStore.
type Refresher struct {
	Collectors []facts.Collector
	Store      store.FactsStore
}

// NewRefresher returns a Refresher that runs collectors and saves results
// to s.
func NewRefresher(collectors []facts.Collector, s store.FactsStore) *Refresher {
	return &Refresher{Collectors: collectors, Store: s}
}

// Start collects facts for every server in serverIDs, once immediately and
// then again every interval, until ctx is canceled. Start blocks; callers
// that want it running in the background should call it in its own
// goroutine (`go refresher.Start(ctx, interval, ids)`).
//
// A single server's collection failure is logged and skipped; it never
// stops the rest of the batch or the ticker itself.
func (r *Refresher) Start(ctx context.Context, interval time.Duration, serverIDs []string) {
	r.refreshAll(ctx, serverIDs)

	ticker := time.NewTicker(interval)
	defer ticker.Stop()

	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			r.refreshAll(ctx, serverIDs)
		}
	}
}

func (r *Refresher) refreshAll(ctx context.Context, serverIDs []string) {
	for _, id := range serverIDs {
		f, err := facts.BuildServerFacts(ctx, id, r.Collectors)
		if err != nil {
			log.Printf("rewsr-facts: refresh failed for server %s: %v", id, err)
			continue
		}
		if err := r.Store.SaveFacts(ctx, f); err != nil {
			log.Printf("rewsr-facts: saving facts for server %s: %v", id, err)
		}
	}
}
