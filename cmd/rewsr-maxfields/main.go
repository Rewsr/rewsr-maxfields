// Command rewsr-maxfields reports what a bare-metal Linux host can actually do.
//
// Run it with no arguments to scan the local host and print a capability
// report. Run `rewsr-maxfields serve` to run the collection pipeline as an
// HTTP API backed by SQLite, refreshing facts on an interval.
//
//	rewsr-maxfields              scan this host, human-readable
//	rewsr-maxfields scan --json  scan this host, machine-readable
//	rewsr-maxfields serve        run the facts API (see env vars below)
package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"os/signal"
	"sort"
	"strings"
	"syscall"
	"time"

	"github.com/Rewsr/rewsr-maxfields/api"
	"github.com/Rewsr/rewsr-maxfields/collectors"
	"github.com/Rewsr/rewsr-maxfields/facts"
	"github.com/Rewsr/rewsr-maxfields/refresh"
	"github.com/Rewsr/rewsr-maxfields/runner"
	"github.com/Rewsr/rewsr-maxfields/store"
)

func main() {
	cmd := "scan"
	args := os.Args[1:]
	if len(args) > 0 && !strings.HasPrefix(args[0], "-") {
		cmd, args = args[0], args[1:]
	}

	switch cmd {
	case "scan":
		runScan(args)
	case "serve":
		if err := runServe(); err != nil {
			log.Fatal(err)
		}
	case "-h", "--help", "help":
		usage()
	default:
		fmt.Fprintf(os.Stderr, "unknown command: %s\n\n", cmd)
		usage()
		os.Exit(1)
	}
}

func usage() {
	fmt.Fprintln(os.Stderr, `rewsr-maxfields - what this bare-metal host can actually do

Usage:
  rewsr-maxfields              scan the local host and print a capability report
  rewsr-maxfields scan --json  scan the local host and print JSON
  rewsr-maxfields serve        run the facts collection API (SQLite-backed)`)
}

// runScan collects facts from the local host once and prints them. It runs
// the Linux host collector plus the deep wave-two hardware collectors, all
// of which degrade gracefully on hosts missing a given tool or subsystem, so
// this is safe to run anywhere (it just returns less on non-Linux hosts).
func runScan(args []string) {
	fs := flag.NewFlagSet("scan", flag.ExitOnError)
	asJSON := fs.Bool("json", false, "print the scan as JSON instead of a report")
	verbose := fs.Bool("verbose", false, "log every collector that could not run (missing tool or subsystem)")
	_ = fs.Parse(args)

	// A one-shot scan on a host that is missing tools is the normal case, not
	// an error worth 14 log lines before the report. Stay quiet unless asked.
	if !*verbose {
		log.SetOutput(io.Discard)
	}

	host, _ := os.Hostname()
	if host == "" {
		host = "localhost"
	}

	r := runner.NewLocalRunner()
	collectorList := append([]facts.Collector{collectors.NewLinuxHostCollector(r)}, collectors.Wave2Collectors(r)...)

	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	// Partial facts plus an error is the normal, expected result on a host
	// that is missing some tools; the report is built from whatever came back.
	f, _ := facts.BuildServerFacts(ctx, host, collectorList)
	if f == nil {
		f = &facts.ServerFacts{ServerID: host}
	}

	if *asJSON {
		printJSON(f)
		return
	}
	printFactsReport(f)
}

func printFactsReport(f *facts.ServerFacts) {
	fmt.Printf("rewsr-maxfields  %s\n", f.ServerID)
	fmt.Printf("  scanned %s   health=%s\n", f.ObservedAt.Format(time.RFC3339), orNA(f.Health))

	hadHardware := f.CPU.Model != "" || f.CPU.Cores > 0 || f.MemoryGB > 0 || len(f.Disks) > 0 || len(f.NICs) > 0
	if hadHardware {
		fmt.Println()
	}
	if f.CPU.Model != "" || f.CPU.Cores > 0 {
		fmt.Printf("  CPU      %s %s  %dS / %dC / %dT\n",
			f.CPU.Vendor, f.CPU.Model, f.CPU.Sockets, f.CPU.Cores, f.CPU.Threads)
	}
	if f.MemoryGB > 0 {
		fmt.Printf("  Memory   %d GB\n", f.MemoryGB)
	}
	for _, d := range f.Disks {
		fmt.Printf("  Disk     %s  %s  %d GB\n", d.Name, orNA(d.Type), d.SizeGB)
	}
	for _, n := range f.NICs {
		feats := ""
		if len(n.Features) > 0 {
			feats = "  [" + strings.Join(n.Features, " ") + "]"
		}
		fmt.Printf("  NIC      %s  %d Gbps  %s%s\n", n.Name, n.SpeedGbps, orNA(n.Driver), feats)
	}

	fmt.Println()
	if len(f.Capabilities) == 0 {
		fmt.Println("  Capabilities  none detected (this tool targets bare-metal Linux;")
		fmt.Println("                run it on the real host to see RDMA, NUMA, GPU, and more)")
	} else {
		caps := append([]string(nil), f.Capabilities...)
		sort.Strings(caps)
		fmt.Printf("  Capabilities  %s\n", strings.Join(caps, "  "))
	}
	if len(f.NetworkModes) > 0 {
		fmt.Printf("  Network       %s\n", strings.Join(f.NetworkModes, "  "))
	}
	fmt.Println()
}

func orNA(s string) string {
	if s == "" {
		return "n/a"
	}
	return s
}

func printJSON(v any) {
	enc := json.NewEncoder(os.Stdout)
	enc.SetIndent("", "  ")
	if err := enc.Encode(v); err != nil {
		fmt.Fprintf(os.Stderr, "encode failed: %v\n", err)
		os.Exit(1)
	}
}

// runServe runs the bare-metal facts collection pipeline and serves the
// curated facts API: collectors gather real facts on an interval, a SQLite
// store persists them, and the HTTP API serves cached facts instead of
// probing hardware per request.
func runServe() error {
	dbPath := envOr("REWSR_MAXFIELDS_DB_PATH", "./rewsr-maxfields.db")
	port := envOr("PORT", "8080")
	refreshInterval := envDuration("REWSR_MAXFIELDS_REFRESH_INTERVAL", 60*time.Second)
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
			log.Printf("rewsr-maxfields: shutdown: %v", err)
		}
	}()

	log.Printf("rewsr-maxfields API listening on :%s (db=%s, refresh=%s, servers=%v)", port, dbPath, refreshInterval, serverIDs)
	if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		return err
	}
	return nil
}

// buildCollectors wires the full collector set for the serving pipeline: the
// Linux host collector, the deep wave-two hardware collectors, a Redfish
// collector (real HTTP, demo fallback if no BMC is reachable), and a
// provisioning collector (reads our own provisioning store).
func buildCollectors(provisioningStore store.ProvisioningStore) []facts.Collector {
	localRunner := runner.NewLocalRunner()

	redfishBaseURL := envOr("REWSR_MAXFIELDS_REDFISH_BASE_URL", "https://localhost:8443")
	redfishToken := os.Getenv("REWSR_MAXFIELDS_REDFISH_TOKEN")

	list := []facts.Collector{
		collectors.NewLinuxHostCollector(localRunner),
		collectors.NewRedfishCollector(redfishBaseURL, redfishToken),
		collectors.NewProvisioningCollector(provisioningStore),
	}
	return append(list, collectors.Wave2Collectors(localRunner)...)
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
		log.Printf("rewsr-maxfields: invalid %s=%q, using default %s: %v", key, v, fallback, err)
		return fallback
	}
	return d
}

// envServerIDs returns the set of server IDs the refresher collects for.
// REWSR_MAXFIELDS_SERVER_IDS is a comma-separated list; with nothing set we
// fall back to a single demo server ID so a fresh checkout is runnable
// end to end without extra setup.
func envServerIDs() []string {
	v := os.Getenv("REWSR_MAXFIELDS_SERVER_IDS")
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
