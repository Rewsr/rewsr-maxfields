package facts

import (
	"context"
	"fmt"
	"log"
	"strings"
	"time"
)

// BuildServerFacts runs every collector for serverID and merges their
// output into one ServerFacts. Collectors run in the order given; a field
// set by an earlier collector is never overwritten by a later one, later
// collectors only fill in gaps. A collector that errors is logged and
// skipped rather than aborting the whole build, since partial facts from
// the remaining collectors are still useful. If every collector fails,
// BuildServerFacts returns a nil ServerFacts and a combined error.
func BuildServerFacts(ctx context.Context, serverID string, collectors []Collector) (*ServerFacts, error) {
	merged := &ServerFacts{
		ServerID: serverID,
		Raw:      map[string]any{},
	}

	var errs []error
	successes := 0

	for _, c := range collectors {
		cf, err := c.Collect(ctx, serverID)
		if err != nil {
			errs = append(errs, err)
			log.Printf("rewsr-facts: collector failed for server %s: %v", serverID, err)
		}
		if cf != nil {
			mergeInto(merged, cf)
			successes++
		}
	}

	if successes == 0 {
		return nil, fmt.Errorf("all collectors failed for server %s: %w", serverID, joinErrors(errs))
	}

	merged.ServerID = serverID
	merged.ObservedAt = time.Now().UTC()
	merged.Capabilities = InferCapabilities(merged)
	merged.Health = InferHealth(merged)

	return merged, nil
}

// mergeInto merges src into dst field by field. A field already set on dst
// is left alone; a field still at its zero value on dst is filled in from
// src if src has a value. This is intentionally explicit per field rather
// than a generic reflect-based merge, so the merge behavior for each field
// is easy to read and change.
func mergeInto(dst, src *ServerFacts) {
	if dst.ServerID == "" {
		dst.ServerID = src.ServerID
	}
	if dst.ObservedAt.IsZero() {
		dst.ObservedAt = src.ObservedAt
	}
	dst.Source = mergeSource(dst.Source, src.Source)
	if dst.PowerState == "" {
		dst.PowerState = src.PowerState
	}
	if dst.Provisioning == "" {
		dst.Provisioning = src.Provisioning
	}

	mergeCPU(&dst.CPU, src.CPU)

	if dst.MemoryGB == 0 {
		dst.MemoryGB = src.MemoryGB
	}
	if len(dst.Disks) == 0 && len(src.Disks) > 0 {
		dst.Disks = src.Disks
	}
	if len(dst.NICs) == 0 && len(src.NICs) > 0 {
		dst.NICs = src.NICs
	}

	dst.NetworkModes = unionStrings(dst.NetworkModes, src.NetworkModes)
	dst.Capabilities = unionStrings(dst.Capabilities, src.Capabilities)

	if dst.FailureDomain == "" {
		dst.FailureDomain = src.FailureDomain
	}

	if src.Raw != nil {
		if dst.Raw == nil {
			dst.Raw = map[string]any{}
		}
		for k, v := range src.Raw {
			if _, exists := dst.Raw[k]; !exists {
				dst.Raw[k] = v
			}
		}
	}
}

func mergeCPU(dst *CPUFacts, src CPUFacts) {
	if dst.Vendor == "" {
		dst.Vendor = src.Vendor
	}
	if dst.Model == "" {
		dst.Model = src.Model
	}
	if dst.Sockets == 0 {
		dst.Sockets = src.Sockets
	}
	if dst.Cores == 0 {
		dst.Cores = src.Cores
	}
	if dst.Threads == 0 {
		dst.Threads = src.Threads
	}
}

// mergeSource records which collectors actually contributed facts, joined
// with "+" (for example "linux_host+redfish"). This is useful for
// debugging where a given field came from; it is not exposed publicly.
func mergeSource(dst, src string) string {
	if src == "" {
		return dst
	}
	if dst == "" {
		return src
	}
	for _, part := range strings.Split(dst, "+") {
		if part == src {
			return dst
		}
	}
	return dst + "+" + src
}

func unionStrings(dst, src []string) []string {
	if len(src) == 0 {
		return dst
	}
	seen := make(map[string]bool, len(dst))
	for _, v := range dst {
		seen[v] = true
	}
	out := dst
	for _, v := range src {
		if v == "" || seen[v] {
			continue
		}
		seen[v] = true
		out = append(out, v)
	}
	return out
}

func joinErrors(errs []error) error {
	if len(errs) == 0 {
		return nil
	}
	msgs := make([]string, 0, len(errs))
	for _, e := range errs {
		msgs = append(msgs, e.Error())
	}
	return fmt.Errorf("%s", strings.Join(msgs, "; "))
}
