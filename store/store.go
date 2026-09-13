// Package store persists ServerFacts (and the provisioning state that
// feeds ProvisioningCollector) to a local SQLite database. It is the only
// package that talks to the database directly; everything else goes
// through the FactsStore and ProvisioningStore interfaces.
package store

import (
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"time"

	"github.com/Rewsr/rewsr-maxfields/facts"
	_ "github.com/mattn/go-sqlite3" // SQLite driver
)

// FactsStore persists merged ServerFacts and serves the latest one back.
// The API layer reads from a FactsStore, never from a Collector, so a
// request never blocks on live hardware probing.
type FactsStore interface {
	SaveFacts(ctx context.Context, f *facts.ServerFacts) error
	GetLatestFacts(ctx context.Context, serverID string) (*facts.ServerFacts, error)
}

// SQLiteStore is a FactsStore backed by a local SQLite database, using
// mattn/go-sqlite3 (the same driver rewsr-complete already uses for local
// storage in cmd/serve.go).
type SQLiteStore struct {
	db *sql.DB
}

// Open opens (creating if needed) a SQLite database at path and prepares
// the server_facts table.
func Open(path string) (*sql.DB, error) {
	db, err := sql.Open("sqlite3", path)
	if err != nil {
		return nil, fmt.Errorf("store: opening %s: %w", path, err)
	}
	if err := db.Ping(); err != nil {
		db.Close()
		return nil, fmt.Errorf("store: connecting to %s: %w", path, err)
	}
	return db, nil
}

// NewSQLiteStore wraps an existing *sql.DB (see Open) as a FactsStore and
// ensures its table exists.
func NewSQLiteStore(db *sql.DB) (*SQLiteStore, error) {
	const schema = `
	CREATE TABLE IF NOT EXISTS server_facts (
		id          INTEGER PRIMARY KEY AUTOINCREMENT,
		server_id   TEXT NOT NULL,
		observed_at DATETIME NOT NULL,
		data        TEXT NOT NULL
	);
	CREATE INDEX IF NOT EXISTS idx_server_facts_server_observed
		ON server_facts(server_id, observed_at);
	`
	if _, err := db.Exec(schema); err != nil {
		return nil, fmt.Errorf("store: creating server_facts schema: %w", err)
	}
	return &SQLiteStore{db: db}, nil
}

// SaveFacts appends a new facts row for f.ServerID. Rows are append-only
// (one per collection cycle) rather than upserted in place, so
// GetLatestFacts is a real "most recent by observed_at" query against the
// indexed column instead of a no-op lookup against a single row.
func (s *SQLiteStore) SaveFacts(ctx context.Context, f *facts.ServerFacts) error {
	if f == nil {
		return fmt.Errorf("store: SaveFacts called with nil facts")
	}
	if f.ServerID == "" {
		return fmt.Errorf("store: SaveFacts called with empty ServerID")
	}

	data, err := json.Marshal(f)
	if err != nil {
		return fmt.Errorf("store: marshaling facts for %s: %w", f.ServerID, err)
	}

	observedAt := f.ObservedAt
	if observedAt.IsZero() {
		observedAt = time.Now().UTC()
	}

	_, err = s.db.ExecContext(ctx,
		`INSERT INTO server_facts (server_id, observed_at, data) VALUES (?, ?, ?)`,
		f.ServerID, observedAt, string(data),
	)
	if err != nil {
		return fmt.Errorf("store: saving facts for %s: %w", f.ServerID, err)
	}
	return nil
}

// GetLatestFacts returns the most recently observed facts for serverID, or
// an error if none are on record.
func (s *SQLiteStore) GetLatestFacts(ctx context.Context, serverID string) (*facts.ServerFacts, error) {
	row := s.db.QueryRowContext(ctx,
		`SELECT data FROM server_facts
		 WHERE server_id = ?
		 ORDER BY observed_at DESC
		 LIMIT 1`,
		serverID,
	)

	var data string
	if err := row.Scan(&data); err != nil {
		if err == sql.ErrNoRows {
			return nil, fmt.Errorf("store: no facts on record for server %s", serverID)
		}
		return nil, fmt.Errorf("store: querying facts for %s: %w", serverID, err)
	}

	var f facts.ServerFacts
	if err := json.Unmarshal([]byte(data), &f); err != nil {
		return nil, fmt.Errorf("store: decoding facts for %s: %w", serverID, err)
	}
	return &f, nil
}
