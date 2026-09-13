try it: `go run ./cmd/rewsr-maxfields` scans this box and prints what it can do. add `--json` if you want to parse it, or `serve` to run the api.

- tells you what a bare-metal linux box actually is: nics and their speeds, numa, pcie gen, rdma, gpu/dpu, hugepages, iommu, the stuff that decides if it goes fast.
- reads cheap sources like /sys, /proc, redfish, and normal linux tools. if a tool isn't installed it skips that bit instead of dying.
- merges it all into one capability list per box, so you're not reading sku strings and guessing.
- for people running a lot of bare metal who want to know what each box can really do, not what the invoice says.
