package store

import (
	"context"
	"slices"
	"testing"
	"time"

	"github.com/Rewsr/rewsr-maxfields/facts"
)

func memoryStore(t *testing.T) *SQLiteStore {
	t.Helper()
	db, err := Open(":memory:")
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	t.Cleanup(func() { db.Close() })

	s, err := NewSQLiteStore(db)
	if err != nil {
		t.Fatalf("NewSQLiteStore: %v", err)
	}
	return s
}

func saveFacts(t *testing.T, s *SQLiteStore, id string, memGB int, observed time.Time) {
	t.Helper()
	err := s.SaveFacts(context.Background(), &facts.ServerFacts{
		ServerID:   id,
		ObservedAt: observed,
		MemoryGB:   memGB,
		Health:     "healthy",
	})
	if err != nil {
		t.Fatalf("SaveFacts(%s): %v", id, err)
	}
}

func TestListServerIDs(t *testing.T) {
	s := memoryStore(t)
	now := time.Now().UTC()

	saveFacts(t, s, "srv-b", 128, now)
	saveFacts(t, s, "srv-a", 256, now)
	saveFacts(t, s, "srv-a", 256, now.Add(time.Minute))

	ids, err := s.ListServerIDs(context.Background())
	if err != nil {
		t.Fatalf("ListServerIDs: %v", err)
	}
	if !slices.Equal(ids, []string{"srv-a", "srv-b"}) {
		t.Errorf("ids = %v", ids)
	}
}

func TestGetAllLatestFactsPicksNewestRow(t *testing.T) {
	s := memoryStore(t)
	base := time.Now().UTC()

	saveFacts(t, s, "srv-1", 100, base)
	saveFacts(t, s, "srv-1", 200, base.Add(time.Minute))
	// Same observed_at as the previous row: the later insert must still
	// win via the row id tiebreak.
	saveFacts(t, s, "srv-1", 300, base.Add(time.Minute))
	saveFacts(t, s, "srv-2", 512, base)

	all, err := s.GetAllLatestFacts(context.Background())
	if err != nil {
		t.Fatalf("GetAllLatestFacts: %v", err)
	}
	if len(all) != 2 {
		t.Fatalf("got %d servers", len(all))
	}
	if all[0].ServerID != "srv-1" || all[0].MemoryGB != 300 {
		t.Errorf("srv-1 latest = %+v", all[0])
	}
	if all[1].ServerID != "srv-2" || all[1].MemoryGB != 512 {
		t.Errorf("srv-2 latest = %+v", all[1])
	}
}

func TestGetAllLatestFactsEmptyStore(t *testing.T) {
	s := memoryStore(t)
	all, err := s.GetAllLatestFacts(context.Background())
	if err != nil {
		t.Fatalf("GetAllLatestFacts: %v", err)
	}
	if len(all) != 0 {
		t.Errorf("empty store returned %d rows", len(all))
	}

	ids, err := s.ListServerIDs(context.Background())
	if err != nil || len(ids) != 0 {
		t.Errorf("ids = %v, %v", ids, err)
	}
}
