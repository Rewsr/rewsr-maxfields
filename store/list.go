package store

import (
	"context"
	"encoding/json"
	"fmt"

	"github.com/Rewsr/rewsr-facts/facts"
)

// FleetLister extends the single-server FactsStore reads with the
// fleet-wide queries the aggregation layer needs. SQLiteStore implements
// both; the interfaces stay separate so the per-server API keeps its
// narrow dependency.
type FleetLister interface {
	ListServerIDs(ctx context.Context) ([]string, error)
	GetAllLatestFacts(ctx context.Context) ([]*facts.ServerFacts, error)
}

// ListServerIDs returns every server that has at least one facts row,
// sorted by id.
func (s *SQLiteStore) ListServerIDs(ctx context.Context) ([]string, error) {
	rows, err := s.db.QueryContext(ctx,
		`SELECT DISTINCT server_id FROM server_facts ORDER BY server_id`)
	if err != nil {
		return nil, fmt.Errorf("store: listing server ids: %w", err)
	}
	defer rows.Close()

	var ids []string
	for rows.Next() {
		var id string
		if err := rows.Scan(&id); err != nil {
			return nil, fmt.Errorf("store: scanning server id: %w", err)
		}
		ids = append(ids, id)
	}
	return ids, rows.Err()
}

// GetAllLatestFacts returns the most recent facts row per server, ordered
// by server id. Latest is decided by the autoincrement row id rather than
// observed_at so two rows written in the same clock tick still have a
// deterministic winner (the later insert).
func (s *SQLiteStore) GetAllLatestFacts(ctx context.Context) ([]*facts.ServerFacts, error) {
	rows, err := s.db.QueryContext(ctx, `
		SELECT sf.data
		FROM server_facts sf
		JOIN (
			SELECT server_id, MAX(id) AS max_id
			FROM server_facts
			GROUP BY server_id
		) latest ON sf.id = latest.max_id
		ORDER BY sf.server_id`)
	if err != nil {
		return nil, fmt.Errorf("store: querying latest fleet facts: %w", err)
	}
	defer rows.Close()

	var all []*facts.ServerFacts
	for rows.Next() {
		var data string
		if err := rows.Scan(&data); err != nil {
			return nil, fmt.Errorf("store: scanning fleet facts: %w", err)
		}
		var f facts.ServerFacts
		if err := json.Unmarshal([]byte(data), &f); err != nil {
			return nil, fmt.Errorf("store: decoding fleet facts: %w", err)
		}
		all = append(all, &f)
	}
	return all, rows.Err()
}
