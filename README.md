#QEMU IMPLEMENTATION FOR OX CONTROLLER

This QEMU version implements the OX Controller running in the DFC Card, exposing it as Open-Channel SSD.

Compiling QEMU:

```
$ ./configure
$ make -j16
```

Please use the follow command to run QEMU (change yout paths and resources) with OX:
Install Linux in the image and use the kernel in the repository below.

```
sudo ~/git_phd/qemu-ox/x86_64-softmmu/qemu-system-x86_64 -monitor stdio -m 16G -smp 6 -s -drive file=/home/nvme-lab/git_phd/qemu-nvme/disk_images/ubuntuimg,id=diskdrive,format=raw,if=none -device ide-hd,drive=diskdrive -device ox-ctrl,serial=deadbeef -serial pty --enable-kvm
```
You can run some tests from the host using the follow Linux kernel and liblightnvm (user space library for Open-Channel SSDs):
```
https://github.com/ivpi/linux-liblnvm

https://github.com/OpenChannelSSD/liblightnvm
```

OX in QEMU comes with VOLT, a Media Manager that implements volatile storage, for now all the data stored in the virtual Open-Channel SSD are gone when you close QEMU. The device has a fixed geometry for now:

```
Channels:           8
Luns per Channel    4
Blocks per Lun      32
Pages per Block     128
Sector per Page     4
Planes              2
Page Size           16 KB
Sector Size         4 KB
OOB Size per Page   1 KB

Total of 4352 MB of volatile Open-Channel SSD. You need enough memory available when you start QEMU.
```

IMPORTANTE: Please use the Linux kernel 4.8 or higher with the NVME driver for the DFC:

```
https://github.com/ivpi/nvme-driver-DFC
```
A default debug is enable, you can see all Admin and IO commands being printed in the host machine, redirect it to a file or to /dev/null if you don't want to print it. An example of IO command debug:

```
[15153] IO CMD 0x91, nsid: 1, cid: 719
 Number of sectors: 8
 DMA size: 32768 (data) + 0 (meta) = 32768 bytes
 [ppa(0): ch: 0, lun: 0, blk: 25, pl: 0, pg: 0, sec: 0]
 [ppa(1): ch: 0, lun: 0, blk: 25, pl: 0, pg: 0, sec: 1]
 [ppa(2): ch: 0, lun: 0, blk: 25, pl: 0, pg: 0, sec: 2]
 [ppa(3): ch: 0, lun: 0, blk: 25, pl: 0, pg: 0, sec: 3]
 [ppa(4): ch: 0, lun: 0, blk: 25, pl: 1, pg: 0, sec: 0]
 [ppa(5): ch: 0, lun: 0, blk: 25, pl: 1, pg: 0, sec: 1]
 [ppa(6): ch: 0, lun: 0, blk: 25, pl: 1, pg: 0, sec: 2]
 [ppa(7): ch: 0, lun: 0, blk: 25, pl: 1, pg: 0, sec: 3]
 [prp(0): 0x00000003f3278000
 [prp(1): 0x00000000110e4000
 [prp(2): 0x00000003f01a8000
 [prp(3): 0x00000003ee194000
 [prp(4): 0x00000000110e4000
 [prp(5): 0x00000003f0c81000
 [prp(6): 0x00000003ec21c000
 [prp(7): 0x00000003e96a0000
 [meta_prp(0): 0x0000000000000000
 CMD cid: 719, type: 0x3 submitted to FTL. 
  Channel: 0, FTL queue 0
 MMGR_CMD type: 0x3 submitted to VOLT.
  Channel: 0, lun: 0, blk: 25, pl: 0, pg: 0]
 MMGR_CMD type: 0x3 submitted to VOLT.
  Channel: 0, lun: 0, blk: 25, pl: 1, pg: 0]
 [IO CALLBK. CMD 0x3. mmgr_ch: 0, lun: 0, blk: 25, pl: 0, pg: 0]
 [IO CALLBK. CMD 0x3. mmgr_ch: 0, lun: 0, blk: 25, pl: 1, pg: 0]
 [NVMe cmd 0x91. cid: 719 completed. Status: 0]
```

#QEMU Original Makefile:

QEMU is a generic and open source machine & userspace emulator and
virtualizer.

QEMU is capable of emulating a complete machine in software without any
need for hardware virtualization support. By using dynamic translation,
it achieves very good performance. QEMU can also integrate with the Xen
and KVM hypervisors to provide emulated hardware while allowing the
hypervisor to manage the CPU. With hypervisor support, QEMU can achieve
near native performance for CPUs. When QEMU emulates CPUs directly it is
capable of running operating systems made for one machine (e.g. an ARMv7
board) on a different machine (e.g. an x86_64 PC board).

QEMU is also capable of providing userspace API virtualization for Linux
and BSD kernel interfaces. This allows binaries compiled against one
architecture ABI (e.g. the Linux PPC64 ABI) to be run on a host using a
different architecture ABI (e.g. the Linux x86_64 ABI). This does not
involve any hardware emulation, simply CPU and syscall emulation.

QEMU aims to fit into a variety of use cases. It can be invoked directly
by users wishing to have full control over its behaviour and settings.
It also aims to facilitate integration into higher level management
layers, by providing a stable command line interface and monitor API.
It is commonly invoked indirectly via the libvirt library when using
open source applications such as oVirt, OpenStack and virt-manager.

QEMU as a whole is released under the GNU General Public License,
version 2. For full licensing details, consult the LICENSE file.


Building
========

QEMU is multi-platform software intended to be buildable on all modern
Linux platforms, OS-X, Win32 (via the Mingw64 toolchain) and a variety
of other UNIX targets. The simple steps to build QEMU are:

  mkdir build
  cd build
  ../configure
  make

Complete details of the process for building and configuring QEMU for
all supported host platforms can be found in the qemu-tech.html file.
Additional information can also be found online via the QEMU website:

  http://qemu-project.org/Hosts/Linux
  http://qemu-project.org/Hosts/W32


Submitting patches
==================

The QEMU source code is maintained under the GIT version control system.

   git clone git://git.qemu-project.org/qemu.git

When submitting patches, the preferred approach is to use 'git
format-patch' and/or 'git send-email' to format & send the mail to the
qemu-devel@nongnu.org mailing list. All patches submitted must contain
a 'Signed-off-by' line from the author. Patches should follow the
guidelines set out in the HACKING and CODING_STYLE files.

Additional information on submitting patches can be found online via
the QEMU website

  http://qemu-project.org/Contribute/SubmitAPatch
  http://qemu-project.org/Contribute/TrivialPatches


Bug reporting
=============

The QEMU project uses Launchpad as its primary upstream bug tracker. Bugs
found when running code built from QEMU git or upstream released sources
should be reported via:

  https://bugs.launchpad.net/qemu/

If using QEMU via an operating system vendor pre-built binary package, it
is preferable to report bugs to the vendor's own bug tracker first. If
the bug is also known to affect latest upstream code, it can also be
reported via launchpad.

For additional information on bug reporting consult:

  http://qemu-project.org/Contribute/ReportABug


Contact
=======

The QEMU community can be contacted in a number of ways, with the two
main methods being email and IRC

 - qemu-devel@nongnu.org
   http://lists.nongnu.org/mailman/listinfo/qemu-devel
 - #qemu on irc.oftc.net

Information on additional methods of contacting the community can be
found online via the QEMU website:

  http://qemu-project.org/Contribute/StartHere

-- End
