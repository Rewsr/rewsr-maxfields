package collectors

import (
	"context"
	"os"
	"path/filepath"
	"slices"
	"testing"
)

// epycCpuinfo is the shape of one processor stanza on an SEV-SNP capable
// EPYC host, trimmed to the fields that matter plus a realistic flags line.
const epycCpuinfo = `processor	: 0
vendor_id	: AuthenticAMD
cpu family	: 25
model		: 1
model name	: AMD EPYC 7763 64-Core Processor
stepping	: 1
microcode	: 0xa0011d1
cpu MHz		: 2450.000
cache size	: 512 KB
flags		: fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat pse36 clflush mmx fxsr sse sse2 ht syscall nx mmxext fxsr_opt pdpe1gb rdtscp lm constant_tsc rep_good nopl nonstop_tsc cpuid extd_apicid aperfmperf rapl pni pclmulqdq monitor ssse3 fma cx16 pcid sse4_1 sse4_2 movbe popcnt aes xsave avx f16c rdrand lahf_lm cmp_legacy svm extapic cr8_legacy abm sse4a misalignsse 3dnowprefetch osvw ibs skinit wdt tce topoext perfctr_core perfctr_nb bpext perfctr_llc mwaitx cpb cat_l3 cdp_l3 invpcid_single hw_pstate ssbd mba ibrs ibpb stibp vmmcall fsgsbase bmi1 avx2 smep bmi2 erms invpcid cqm rdt_a rdseed adx smap clflushopt clwb sha_ni xsaveopt xsavec xgetbv1 xsaves cqm_llc cqm_occup_llc cqm_mbm_total cqm_mbm_local clzero irperf xsaveerptr rdpru wbnoinvd amd_ppin arat npt lbrv svm_lock nrip_save tsc_scale vmcb_clean flushbyasid decodeassists pausefilter pfthreshold avic v_vmsave_vmload vgif v_spec_ctrl umip pku ospke vaes vpclmulqdq rdpid overflow_recov succor smca sme sev sev_es sev_snp
bugs		: sysret_ss_attrs spectre_v1 spectre_v2 spec_store_bypass
bogomips	: 4900.00
`

// writeTree writes a map of relative path to content under root, creating
// directories as needed.
func writeTree(t *testing.T, root string, files map[string]string) {
	t.Helper()
	for rel, content := range files {
		full := filepath.Join(root, rel)
		if err := os.MkdirAll(filepath.Dir(full), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(full, []byte(content), 0o644); err != nil {
			t.Fatal(err)
		}
	}
}

func TestTEECollectorSEVSNPHost(t *testing.T) {
	root := t.TempDir()
	writeTree(t, root, map[string]string{
		"proc/cpuinfo":                          epycCpuinfo,
		"sys/module/kvm_amd/parameters/sev":     "Y\n",
		"sys/module/kvm_amd/parameters/sev_es":  "Y\n",
		"sys/module/kvm_amd/parameters/sev_snp": "Y\n",
		"dev/sev":                               "",
	})

	c := &TEECollector{SysRoot: root}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}

	for _, want := range []string{"sev", "sev_es", "sev_snp", "tee_capable"} {
		if !slices.Contains(f.Capabilities, want) {
			t.Errorf("capabilities missing %q: %v", want, f.Capabilities)
		}
	}
	if slices.Contains(f.Capabilities, "tdx") {
		t.Errorf("tdx should not be reported on an AMD host: %v", f.Capabilities)
	}
	if f.Source != "linux_tee" {
		t.Errorf("source = %q", f.Source)
	}
	if _, ok := f.Raw["tee"]; !ok {
		t.Error("Raw[tee] missing")
	}
}

func TestTEECollectorKVMParamDisablesSEV(t *testing.T) {
	root := t.TempDir()
	writeTree(t, root, map[string]string{
		"proc/cpuinfo":                          epycCpuinfo,
		"sys/module/kvm_amd/parameters/sev":     "N\n",
		"sys/module/kvm_amd/parameters/sev_es":  "N\n",
		"sys/module/kvm_amd/parameters/sev_snp": "N\n",
	})

	c := &TEECollector{SysRoot: root}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}

	for _, banned := range []string{"sev", "sev_es", "sev_snp", "tee_capable"} {
		if slices.Contains(f.Capabilities, banned) {
			t.Errorf("capability %q reported despite kvm_amd disabling it: %v", banned, f.Capabilities)
		}
	}
}

func TestTEECollectorFlagWithoutParamStillCounts(t *testing.T) {
	// A host with the CPU flags but no kvm_amd module loaded (kvm not in
	// use) should still report SEV capability: the hardware supports it
	// and nothing observed says it is disabled.
	root := t.TempDir()
	writeTree(t, root, map[string]string{
		"proc/cpuinfo": epycCpuinfo,
	})

	c := &TEECollector{SysRoot: root}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}
	if !slices.Contains(f.Capabilities, "sev_snp") {
		t.Errorf("sev_snp missing without kvm params: %v", f.Capabilities)
	}
}

func TestTEECollectorTDXGuest(t *testing.T) {
	root := t.TempDir()
	writeTree(t, root, map[string]string{
		"proc/cpuinfo": `processor	: 0
vendor_id	: GenuineIntel
model name	: Intel(R) Xeon(R) Platinum 8480+
flags		: fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat pse36 clflush mmx fxsr sse sse2 ss ht syscall nx pdpe1gb rdtscp lm constant_tsc rep_good nopl xtopology nonstop_tsc cpuid tsc_known_freq pni pclmulqdq ssse3 fma cx16 pcid sse4_1 sse4_2 movbe popcnt aes xsave avx f16c rdrand hypervisor lahf_lm abm 3dnowprefetch fsgsbase bmi1 avx2 smep bmi2 erms invpcid avx512f avx512dq rdseed adx smap avx512ifma clflushopt clwb avx512cd sha_ni avx512bw avx512vl xsaveopt xsavec xgetbv1 xsaves tdx_guest arat avx512vbmi umip pku ospke avx512_vbmi2 gfni vaes vpclmulqdq avx512_vnni avx512_bitalg avx512_vpopcntdq rdpid cldemote movdiri movdir64b fsrm md_clear serialize tsxldtrk amx_bf16 avx512_fp16 amx_tile amx_int8
`,
		"dev/tdx_guest": "",
	})

	c := &TEECollector{SysRoot: root}
	f, err := c.Collect(context.Background(), "srv-1")
	if err != nil {
		t.Fatalf("Collect: %v", err)
	}
	if !slices.Contains(f.Capabilities, "tdx") {
		t.Errorf("tdx missing: %v", f.Capabilities)
	}
	if slices.Contains(f.Capabilities, "sev") {
		t.Errorf("sev should not be reported on an Intel guest: %v", f.Capabilities)
	}
}

func TestTEECollectorNoSources(t *testing.T) {
	c := &TEECollector{SysRoot: t.TempDir()}
	f, err := c.Collect(context.Background(), "srv-1")
	if err == nil {
		t.Fatal("expected error when nothing is readable")
	}
	if f == nil {
		t.Fatal("facts must be non-nil even on error")
	}
	if len(f.Capabilities) != 0 {
		t.Errorf("no capabilities expected: %v", f.Capabilities)
	}
}

func TestParseBoolParam(t *testing.T) {
	cases := map[string]bool{
		"Y\n": true, "y": true, "1\n": true,
		"N\n": false, "n": false, "0": false, "": false, "garbage": false,
	}
	for in, want := range cases {
		if got := parseBoolParam(in); got != want {
			t.Errorf("parseBoolParam(%q) = %v, want %v", in, got, want)
		}
	}
}
