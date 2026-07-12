# Collectors

Wave one (wired in main.go): linux_host (lscpu/lsblk/ip/ethtool), redfish, provisioning.

Wave two (collectors.Wave2Collectors, append in buildCollectors):

| Collector | Source | Adds |
|---|---|---|
| linux_pci | lspci -Dnnmm + /sys/bus/pci | NIC/NVMe/accelerator inventory, link gen/width, sriov counts. Caps: pcie_gen4/5, gpu_accelerator, sriov |
| linux_rdma | rdma link -j, ibv_devinfo, /sys/class/infiniband | RDMA ports, IB vs RoCE, fw. Caps: rdma, infiniband, roce, rdma_active |
| linux_numa | numactl --hardware, /sys/devices/system/node | nodes, per-node mem, distance matrix. Cap: multi_numa |
| linux_hugepages | /proc/meminfo, /sys/kernel/mm/hugepages | pool counters per size. Caps: hugepages, hugepages_1g |
| linux_iommu | /sys/kernel/iommu_groups, /proc/cmdline | group count, cmdline flags. Caps: iommu, iommu_passthrough |
| linux_tee | /proc/cpuinfo, kvm module params, /dev nodes | SEV/SNP/TDX/SGX detection. Caps: sev, sev_es, sev_snp, tdx, sgx, tee_capable |
| linux_nic_offload | ethtool -i/-g/-l/-T | driver+fw, rings, channels, hw timestamping. Cap: ptp_hardware_clock |
| linux_net_topology | ip -j -d link, /proc/net/bonding | bonds+LACP, vlans, bridges. Modes: bonded, lacp, vlan_tagged, bridged |
| linux_lldp | lldpctl -f json | switch neighbors, failure-domain hint (switch:NAME) |
| linux_kernel | uname, /proc/cmdline, /proc/modules, /proc/sys | release, cmdline, fabric modules, net.core sysctls. Cap: vfio |
| linux_smbios | /sys/class/dmi/id | vendor/product/serial/BIOS/chassis |
| linux_nvme | nvme list -o json | per-drive model/fw/serial/size |
| ipmi | ipmitool | power state, temps/fans/watts, not-ok sensors |

All degrade per-source: missing tool or subsystem = partial facts + error naming the gap, never a panic. File-reading collectors take SysRoot for fixture-tree tests. Command collectors take the shared CommandRunner and LookPath.
