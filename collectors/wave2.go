package collectors

import (
	"github.com/Rewsr/rewsr-maxfields/facts"
	"github.com/Rewsr/rewsr-maxfields/runner"
)

// Wave2Collectors returns the deep-hardware collector set: PCIe topology,
// RDMA, NUMA, hugepages, IOMMU, TEE capability, per-NIC offload and
// timestamping, interface composition, LLDP neighbors, kernel facts, and
// SMBIOS identity. Everything here degrades gracefully on hosts missing a
// given subsystem or tool, so the whole set is safe to run everywhere the
// wave-one collectors run.
//
// Wiring: append these to the slice built in cmd/rewsr-maxfields/main.go, in
// buildCollectors, after the provisioning collector:
//
//	list := []facts.Collector{ ...existing three... }
//	return append(list, collectors.Wave2Collectors(localRunner)...)
//
// Order matters to the merge only for overlapping fields, and wave two is
// deliberately additive: it fills Raw keys, Capabilities, and NetworkModes
// that wave one never sets, so appending after the existing collectors
// keeps their authority intact (a provisioning failure domain always beats
// the LLDP hint, for example).
func Wave2Collectors(r runner.CommandRunner) []facts.Collector {
	return []facts.Collector{
		NewPCICollector(r),
		NewRDMACollector(r),
		NewNUMACollector(r),
		NewHugepagesCollector(),
		NewIOMMUCollector(),
		NewTEECollector(),
		NewNICOffloadCollector(r),
		NewNetTopologyCollector(r),
		NewLLDPCollector(r),
		NewKernelCollector(r),
		NewSMBIOSCollector(),
		NewNVMeCollector(r),
		NewIPMICollector(r),
		NewGPUCollector(r),
	}
}
