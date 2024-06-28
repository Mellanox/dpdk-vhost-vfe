# VirtIO VF PCI devices for vhost acceleration

Virtio VF PCIe devices can be attached to the guest VM using vhost acceleration software stack. This enables performing live migration of guest VMs.

# Prerequisites

Minimum hypervisor kernel version - Linux kernel 5.7 (for VFIO SR-IOV support).
To use high-availability (the additional vfe-vhostd-ha service which can persist datapath when vfe-vhostd crashes), this linux [kernel patch](https://github.com/torvalds/linux/commit/ffed0518d871482e26c5826c0875bea6775446da)  must be applied on hypervisor.

# Install vHost Acceleration Software Stack

Vhost acceleration software stack is built using open-source BSD licensed DPDK.

## Install vhost acceleration software

1. Clone the software source code, or install previous [releases](https://github.com/Mellanox/dpdk-vhost-vfe/releases).

        [host]# git clone https://github.com/Mellanox/dpdk-vhost-vfe

2. Build software: 

        [host]# apt-get install libev-dev
        [host]# yum install -y numactl-devel libev-devel
        [host]# meson build --debug -Denable_drivers=vdpa/virtio,common/virtio,common/virtio_mi,common/virtio_ha
        [host]# ninja -C build install

3. Start vhostd and vhostd-ha(optional) service

    [host]# systemctl start vfe-vhostd
    [host]# systemctl start vfe-vhostd-ha

## Install QEMU

Upstream QEMU later than 8.1 can be used or the following NVIDIA QEMU.

1. Clone NVIDIA QEMU sources. 

        [host]# git clone https://github.com/Mellanox/qemu -b stable-8.1-presetup

2. Build NVIDIA QEMU. 

        [host]# mkdir bin
        [host]# cd bin
        [host]# ../configure --target-list=x86_64-softmmu --enable-kvm
        [host]# make -j24


# Steps of using a Vnet or Vblk PCIe VF in Virtual Machine

1. Set the DPU nvconfig to enable virtio static PF with SR-IOV VF. Make sure virtio PF exists on host and SR-IOV enabled.

2. Configure libvirt VM XML.  
    Add a virtio-net interface  in VM XML. 

        <qemu:commandline>
        <qemu:arg value='-chardev'/>
        <qemu:arg value='socket,id=char0,path=/tmp/vhost-net0,server=on'/>
        <qemu:arg value='-netdev'/>
        <qemu:arg value='type=vhost-user,id=vhost1,chardev=char0,queues=4'/>
        <qemu:arg value='-device'/>
        <qemu:arg value='virtio-net-pci,netdev=vhost1,mac=00:00:00:00:33:00,vectors=10,page-per-vq=on,rx_queue_size=1024,tx_queue_size=1024,mq=on,disable-legacy=on,disable-modern=off'/>
        </qemu:commandline>

    Add a a virtio-blk interface in VM XML.

        <qemu:commandline>
        <qemu:arg value='-chardev'/>
        <qemu:arg value='socket,id=char1,path=/tmp/vhost-blk0,server=on'/>
        <qemu:arg value='-device'/>
        <qemu:arg value='vhost-user-blk-pci,chardev=char1,page-per-vq=on,num-queues=4,disable-legacy=on,disable-modern=off'/>
        </qemu:commandline>

3. Bind PCIe VF device to vfio-pci driver and enable SR-IOV.
    * Bind the virtio-net PF devices to vfio-pci driver and create 1 virtio-net VF:

            [host]# lspci -s 0000:af:00.2
            af:00.2 Ethernet controller: Red Hat, Inc. Virtio network device (rev 01)
        
            [host]# modprobe vfio vfio_pci
            [host]# echo 1 > /sys/module/vfio_pci/parameters/enable_sriov
            [host]# echo 0x1af4 0x1041 > /sys/bus/pci/drivers/vfio-pci/new_id
            [host]# echo 0000:af:00.2 > /sys/bus/pci/drivers/vfio-pci/bind
            
            [host]# lspci -vvv -s 0000:af:00.2 | grep "Kernel driver"
            Kernel driver in use: vfio-pci
            Enable SR-IOV and create a virtio-net VF(s): 

            [host]# echo 1 > /sys/bus/pci/devices/0000:af:00.2/sriov_numvfs
            
            [host]# lspci | grep Virtio
            af:00.2 Ethernet controller: Red Hat, Inc. Virtio network device
            af:04.5 Ethernet controller: Red Hat, Inc. Virtio network device

            

    * Bind the virtio-blk PF devices to vfio-pci driver and create 1 virtio-blk VF:

            lspci -s 0000:af:00.3
            af:00.3 Non-Volatile memory controller: Red Hat, Inc. Virtio block device (rev 01)
            
            modprobe vfio vfio_pci
            echo 1 > /sys/module/vfio_pci/parameters/enable_sriov
            echo 0x1af4 0x1042 > /sys/bus/pci/drivers/vfio-pci/new_id
            echo 0000:af:00.3 > /sys/bus/pci/drivers/vfio-pci/bind
            
            lspci -vvv -s 0000:af:00.3 | grep "Kernel driver"
            Kernel driver in use: vfio-pci
            Enable SR-IOV and create a virtio-blkVF(s): 

            echo 1 > /sys/bus/pci/devices/0000:af:00.3/sriov_numvfs
            
            lspci | grep Virtio
            af:00.3 Non-Volatile memory controller: Red Hat, Inc. Virtio block device
            af:05.1 Non-Volatile memory controller: Red Hat, Inc. Virtio block device


4. Provision and add VF device to vhostd service:

    * virtio-net VF

        1. Add a VF representor to the OVS bridge on the DPU: 

                [dpu]# virtnet query -p 0 -v 0 | grep sf_rep_net_device
                "sf_rep_net_device": "en3f0pf0sf3000",
                [dpu]# ovs-vsctl add-port ovsbr1 en3f0pf0sf3000

        2. Add PF device and wait virtio-net-controller finishing handle PF FLR:

                [host]# vfe-vhostd-cli mgmtpf -a 0000:af:00.2

        3. Provision VF on DPU(optional): 

                [dpu]# virtnet modify -p 0 -v 0 device -m 00:00:00:00:33:00

        4. Add VF to vhostd service

                [host]# vfe-vhostd-cli vf -a 0000:af:04.5 -v /tmp/vhost-net0

    * virtio-blk VF

        1. Create block device on the DPU: 

                On BlueField-3 SNAP:
                [dpu]# spdk_rpc.py bdev_null_create Null0 1024 512
                [dpu]# snap_rpc.py virtio_blk_controller_create --pf_id 0 --bdev Null0 --num_queues 1 --admin_q
                For BlueField-3 feature: shared memory based recovery(environment variable VBLK_RECOVERY_SHM) is developed to substitute --force_in_order.

                On BlueField-2 SNAP: 

                [dpu]# spdk_rpc.py bdev_null_create Null0 1024 512
                [dpu]# snap_rpc.py controller_virtio_blk_create --pf_id 0 --bdev_type spdk mlx5_0 --bdev Null0 --num_queues 1 --admin_q --force_in_order

        2. Add virtio-blk PF to vhost acceleration service.

                [host]# vfe-vhostd-cli mgmtpf -a 0000:af:00.3
                # Wait on SNAP controller to finish handling PF FLR
                
                # On DPU, the user must create a VF device controller before adding the VF device to the vhostd
                # service or after pf or vf device delete from vhostd service , or vhostd service restart:
                #       For BlueField-3, the VF controller is automatically recreated
                #       For BlueField-2, the VF controller must be manually recreated
                # Use snap_rpc.py controller_list to check for controller exsistence and create controller if it's not there
                [dpu]# snap_rpc.py controller_virtio_blk_create mlx5_0 --pf_id 0 --vf_id 0 --bdev_type spdk --bdev Null0 --force_in_order

        3. Add virtio-blk VF to vhostd service

                [host]# vfe-vhostd-cli vf -a 0000:af:05.1 -v /tmp/vhost-blk0

5. Start the VM

        virsh start <domain-name>

6. Remove Device  
When finished using the virtio device, use following commands to remove them from vhostd service:

        [host]# vfe-vhostd-cli vf -r 0000:af:04.5
        [host]# vfe-vhostd-cli mgmtpf -r 0000:af:00.2  
        
        [host]# vfe-vhostd-cli vf -r 0000:af:05.1
        [host]# vfe-vhostd-cli vf -r 0000:af:00.3

# Simple Live Migration Virtual Machine

Prepare two identical hosts and perform the steps of adding the virtio devices to vhostd service on both server. Boot the virtual machine on one server and live migration it to another server with command like:

    virsh migrate --verbose --live --persistent gen-l-vrt-440-162-CentOS-7.4 qemu+ssh://gen-l-vrt-439/system --unsafe

# Vhost Acceleration Service

## Vfe-vhostd Service

This service communicates to QEMU vhost-user front-end and programs virtio VF through VFIO and virtio driver.

* Start vfe-vhostd service:

        [host]# systemctl start vfe-vhostd

* Stop vfe-vhostd service:

        [host]# systemctl start vfe-vhostd

* Check vfe-vhostd service log:

        [host]# journalctl -u vfe-vhostd

## Vfe-vhostd-ha Service

Running vfe-vhostd-ha service allows datapath to persist in case vfe-vhostd crash. vhostd service and vhostd-ha service will connect each other through unix domain socket. So vhostd-ha service can get information from vhostd service.

* Start/Stop/Log of the vfe-vhostd-ha service

        [host]# systemctl start vfe-vhostd-ha
        [host]# systemctl stop vfe-vhostd-ha
        [host]# journalctl -u vfe-vhostd-ha

## Hot upgrade

After install new software package:

    systemctl restart vfe-vhostd-ha
    systemctl restart vfe-vhostd

## Vfe-vhostd-cli RPC Commands
### Show vfe-vhostd version

    [host]# vfe-vhost-cli version
    {
    "vfe-vhostd version": "DPDK 22.07.0-rc0.mlx_vfe_vdpa-1.2",
    "vfe-vhostd build time": "May 20 2024 18:08:50",
    "vfe-vhostd-ha version": "DPDK 22.07.0-rc0.mlx_vfe_vdpa-1.2",
    "vfe-vhostd-ha build time": "May 20 2024 18:08:50",
    "errno": 0,
    "errstr": "Success"
    }

### Add/Remove/List PF device
Before adding any VF device, PF device must be added in advance. Before removing any PF device, all VF devices must be removed in advance.

    [host]# vfe-vhost-cli mgmtpf -h
    usage: vfe-vhost-cli mgmtpf [-h] [-a | -r | -l] [DEVICE]
    
    positional arguments:
    DEVICE        Device specified as PCI "domain:bus:slot.func" syntax or "bus:slot.func" syntax. For device
                    add/remove to drivers, they may be referred to by interface name.
    
    optional arguments:
    -h, --help    show this help message and exit
    -a, --add     add a pci device
    -r, --remove  remove a pci device
    -l, --list    list all PF devices

### Add/Remove/List VF device

    [host]# vfe-vhost-cli vf -h
    usage: vfe-vhost-cli vf [-h] [-a | -r | -l | -i | -d] [-o test_operation] [-b test_size_mode] [-v vhost_socket]
                            [-u vm_uuid]
                            [DEVICE]
    
    positional arguments:
    DEVICE             Device specified as PCI "domain:bus:slot.func" syntax or "bus:slot.func" syntax. For device
                        add/remove to drivers, they may be referred to by interface name.
    
    optional arguments:
    -h, --help         show this help message and exit
    -a, --add          add a pci device
    -r, --remove       remove a pci device
    -l, --list         list all VF devices of PF device
    -i, --info         show specified VF device information
    -d, --debug        test VF device debug
    -v vhost_socket    Vhost socket file name
    -u vm_uuid         Virtual machine UUID


# QEMU

## Parameters for  QEMU emulated virtio device

### Direct doorbell mapping
QEMU's direct doorbell mapping feature can reduce vmexit when guest driver rings VQ doorbell. To use this feature, virtio-device must work in modern mode and page-per-vq must be set.

### Virtio-net and Virtio-blk properties
The offload properties (host_tso4, csum, ...) or VQ properties (tx_queue_size, rx_queue_size...) should be properly set to QEMU. Full property list can be get from following QEMU command:

    [host]# qemu-system-x86_64 -device virtio-net,help
    [host]# qemu-system-x86_64 -device vhost-user-blk-pci,help

### Vectors and MSIX
For virtio-net, guest driver will assign each TX queue or RX queue a dedicated MSIX interrupt ONLY IF device has enough MSIX resource. Make sure: QEMU virtio-net property **vectors = 2*N_qpair+2**, so QEMU emulated VF device has enough MSIX resource.

### Jambo MTU
Configure virtio-net device MTU on DPU, set QEMU virtio-net device property host_mtu to the same value.

Isolate DPA cores for virtio net and virtio blk 
Limit the arm cores snap use
Through doca_snap.yaml environment variable APP_ARGS:

    env:
      - name: APP_ARGS
        value: "-m 0xfff"

# Isolate DPA cores for both virtio-net controller and virtio-blk controller

Example: vblk(3 cores) and vnet (8 cores).

In virtio-blk controller snap container yaml file /etc/kubelet.d/doca_snap.yaml:
 
    env:
      - name: dpa_virtq_split_core_mask
        value: "0xffffffffffff"
 
In virtio-net controller configuration file /opt/mellanox/mlnx_virtnet/virtnet.conf:
{
  "dpa_core_start": 3,
  "dpa_core_end": 10 
}
For detail refer (link TBD)

# Trouble shooting tips

* Error on host: `Can't bind virtio device to VFIO`.  
Solution: Add intel_iommu=on iommu=pt to /proc/cmdline.

* Error on host: `vfio-pci 0001:86:00.3: can't enable 127 VFs (bus 87 out of range of [bus 86])`  
Solution: Add pci=realloc,assign-busses to /proc/cmdline.

