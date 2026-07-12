package store

import (
	"context"
	"database/sql"
	"fmt"
	"time"
)

// ProvisioningStore holds what our own system already knows about a
// server's provisioning state and failure domain assignment. This is
// separate from FactsStore: FactsStore caches the output of a facts
// collection run, ProvisioningStore is the input ProvisioningCollector
// reads from, since provisioning state is written by our own control
// plane, not observed from the server itself.
type ProvisioningStore interface {
	GetProvisioning(ctx context.Context, serverID string) (provisioning, failureDomain string, err error)
	SetProvisioning(ctx context.Context, serverID, provisioning, failureDomain string) error
}

// SQLiteProvisioningStore is a ProvisioningStore backed by SQLite, using
// the same driver and database file convention as SQLiteStore.
type SQLiteProvisioningStore struct {
	db *sql.DB
}

// NewSQLiteProvisioningStore wraps an existing *sql.DB as a
// ProvisioningStore and ensures its table exists.
func NewSQLiteProvisioningStore(db *sql.DB) (*SQLiteProvisioningStore, error) {
	const schema = `
	CREATE TABLE IF NOT EXISTS provisioning_state (
		server_id      TEXT PRIMARY KEY,
		provisioning   TEXT NOT NULL,
		failure_domain TEXT NOT NULL,
		updated_at     DATETIME NOT NULL
	);
	`
	if _, err := db.Exec(schema); err != nil {
		return nil, fmt.Errorf("store: creating provisioning_state schema: %w", err)
	}
	return &SQLiteProvisioningStore{db: db}, nil
}

// GetProvisioning returns the provisioning state and failure domain on
// record for serverID.
func (s *SQLiteProvisioningStore) GetProvisioning(ctx context.Context, serverID string) (string, string, error) {
	row := s.db.QueryRowContext(ctx,
		`SELECT provisioning, failure_domain FROM provisioning_state WHERE server_id = ?`,
		serverID,
	)

	var provisioning, failureDomain string
	if err := row.Scan(&provisioning, &failureDomain); err != nil {
		if err == sql.ErrNoRows {
			return "", "", fmt.Errorf("store: no provisioning record for server %s", serverID)
		}
		return "", "", fmt.Errorf("store: querying provisioning state for %s: %w", serverID, err)
	}
	return provisioning, failureDomain, nil
}

// SetProvisioning records the provisioning state and failure domain our
// control plane has assigned to serverID. This is how a provisioning
// record gets in front of ProvisioningCollector in the first place; it is
// called by whatever provisions the server, not by the facts pipeline
// itself.
func (s *SQLiteProvisioningStore) SetProvisioning(ctx context.Context, serverID, provisioning, failureDomain string) error {
	if serverID == "" {
		return fmt.Errorf("store: SetProvisioning called with empty serverID")
	}
	_, err := s.db.ExecContext(ctx,
		`INSERT INTO provisioning_state (server_id, provisioning, failure_domain, updated_at)
		 VALUES (?, ?, ?, ?)
		 ON CONFLICT(server_id) DO UPDATE SET
			provisioning = excluded.provisioning,
			failure_domain = excluded.failure_domain,
			updated_at = excluded.updated_at`,
		serverID, provisioning, failureDomain, time.Now().UTC(),
	)
	if err != nil {
		return fmt.Errorf("store: saving provisioning state for %s: %w", serverID, err)
	}
	return nil
}
