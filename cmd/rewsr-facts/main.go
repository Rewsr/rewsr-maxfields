// Command rewsr-facts runs the bare-metal facts collection pipeline and
// serves the curated facts API described in the repo README: collectors
// gather real facts on an interval, a SQLite-backed store persists them,
// and the HTTP API serves cached facts instead of probing hardware per
// request.
package main

import (
	"context"
	"log"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/Rewsr/rewsr-facts/api"
	"github.com/Rewsr/rewsr-facts/collectors"
	"github.com/Rewsr/rewsr-facts/facts"
	"github.com/Rewsr/rewsr-facts/refresh"
	"github.com/Rewsr/rewsr-facts/runner"
	"github.com/Rewsr/rewsr-facts/store"
)

func main() {
	if err := run(); err != nil {
		log.Fatal(err)
	}
}

func run() error {
	dbPath := envOr("REWSR_FACTS_DB_PATH", "./rewsr-facts.db")
	port := envOr("PORT", "8080")
	refreshInterval := envDuration("REWSR_FACTS_REFRESH_INTERVAL", 60*time.Second)
	serverIDs := envServerIDs()

	db, err := store.Open(dbPath)
	if err != nil {
		return err
	}
	defer db.Close()

	factsStore, err := store.NewSQLiteStore(db)
	if err != nil {
		return err
	}

	provisioningStore, err := store.NewSQLiteProvisioningStore(db)
	if err != nil {
		return err
	}

	// Seed a demo provisioning record for the default demo server so a
	// fresh checkout has something to serve out of the box. This is only
	// a fallback for servers with no real provisioning record yet; real
	// deployments call ProvisioningStore.SetProvisioning from their own
	// provisioning control plane.
	for _, id := range serverIDs {
		if _, _, err := provisioningStore.GetProvisioning(context.Background(), id); err != nil {
			_ = provisioningStore.SetProvisioning(context.Background(), id, "provisioned", "demo-fd-1")
		}
	}

	collectorList := buildCollectors(provisioningStore)

	refresher := refresh.NewRefresher(collectorList, factsStore)

	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()

	go refresher.Start(ctx, refreshInterval, serverIDs)

	srv := &http.Server{
		Addr:    ":" + port,
		Handler: api.NewServer(factsStore).Routes(),
	}

	go func() {
		<-ctx.Done()
		shutdownCtx, shutdownCancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer shutdownCancel()
		if err := srv.Shutdown(shutdownCtx); err != nil {
			log.Printf("rewsr-facts: shutdown: %v", err)
		}
	}()

	log.Printf("rewsr-facts API listening on :%s (db=%s, refresh=%s, servers=%v)", port, dbPath, refreshInterval, serverIDs)
	if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		return err
	}
	return nil
}

// buildCollectors wires the three real collectors: a Linux host collector
// (real commands, gracefully degraded if the Linux tools aren't present),
// a Redfish collector (real HTTP, demo fallback if no BMC is reachable),
// and a provisioning collector (reads our own provisioning store).
func buildCollectors(provisioningStore store.ProvisioningStore) []facts.Collector {
	localRunner := runner.NewLocalRunner()

	redfishBaseURL := envOr("REWSR_FACTS_REDFISH_BASE_URL", "https://localhost:8443")
	redfishToken := os.Getenv("REWSR_FACTS_REDFISH_TOKEN")

	return []facts.Collector{
		collectors.NewLinuxHostCollector(localRunner),
		collectors.NewRedfishCollector(redfishBaseURL, redfishToken),
		collectors.NewProvisioningCollector(provisioningStore),
	}
}

func envOr(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}

func envDuration(key string, fallback time.Duration) time.Duration {
	v := os.Getenv(key)
	if v == "" {
		return fallback
	}
	d, err := time.ParseDuration(v)
	if err != nil {
		log.Printf("rewsr-facts: invalid %s=%q, using default %s: %v", key, v, fallback, err)
		return fallback
	}
	return d
}

// envServerIDs returns the set of server IDs the refresher collects for.
// REWSR_FACTS_SERVER_IDS is a comma-separated list; with nothing set we
// fall back to a single demo server ID so a fresh checkout is runnable
// end to end without extra setup.
func envServerIDs() []string {
	v := os.Getenv("REWSR_FACTS_SERVER_IDS")
	if v == "" {
		return []string{"demo-server-1"}
	}
	var ids []string
	for _, id := range strings.Split(v, ",") {
		id = strings.TrimSpace(id)
		if id != "" {
			ids = append(ids, id)
		}
	}
	return ids
}
