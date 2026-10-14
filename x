
=== rvc_cpu ===
[    0.000000] Linux version 5.17.11-rvc+ (shaderemu@shaderemu) (riscv32-linux-gcc.br_real (Buildroot 2021.11-12449-g1bef613319) 13.3.0, GNU ld (GNU Binutils) 2.41) #1 Mon Jan 1 00:00:00 UTC 2024
[    0.000000] OF: fdt: Ignoring memory range 0x80000000 - 0x80400000
[    0.000000] Machine model: generic,rvc
[    0.000000] earlycon: sbi0 at I/O port 0x0 (options '')
[    0.000000] printk: bootconsole [sbi0] enabled
[    0.000000] Zone ranges:
[    0.000000]   Normal   [mem 0x0000000080400000-0x0000000085ffffff]
[    0.000000] Movable zone start for each node
[    0.000000] Early memory node ranges
[    0.000000]   node   0: [mem 0x0000000080400000-0x0000000085ffffff]
[    0.000000] Initmem setup node 0 [mem 0x0000000080400000-0x0000000085ffffff]
[    0.000000] SBI specification v0.2 detected
[    0.000000] SBI implementation ID=0x454d55 Version=0x1
[    0.000000] SBI TIME extension detected
[    0.000000] riscv: ISA extensions aimsu
[    0.000000] riscv: ELF capabilities afim
[    0.000000] Built 1 zonelists, mobility grouping on.  Total pages: 23368
[    0.000000] Kernel command line: root=mtd:root rootfstype=romfs ro init=/emuinit phram.phram=root,0x40000000,220Mi console=hvc0 earlycon=sbi
[    0.000000] Dentry cache hash table entries: 16384 (order: 4, 65536 bytes, linear)
[    0.000000] Inode-cache hash table entries: 8192 (order: 3, 32768 bytes, linear)
[    0.000000] mem auto-init: stack:off, heap alloc:off, heap free:off
[    0.000000] Memory: 88460K/94208K available (2943K kernel code, 508K rwdata, 1085K rodata, 124K init, 201K bss, 5748K reserved, 0K cma-reserved)
[    0.000000] SLUB: HWalign=64, Order=0-3, MinObjects=0, CPUs=1, Nodes=1
[    0.000000] NR_IRQS: 64, nr_irqs: 64, preallocated irqs: 0
[    0.000000] riscv-intc: 32 local interrupts mapped
[    0.000000] random: [_pi_] adding 16 byte of rvc randomness to entropy pool
[    0.000000] riscv_timer_init_dt: Registering clocksource cpuid [0] hartid [0]
[    0.000000] clocksource: riscv_clocksource: mask: 0xffffffffffffffff max_cycles: 0x12e4acc51, max_idle_ns: 451374286889000 ns
[    0.000000] sched_clock: 64 bits at 5kHz, resolution 200000ns, wraps every 562949953400000ns
[    0.004800] printk: console [hvc0] enabled
[    0.004800] printk: console [hvc0] enabled
[    0.007400] printk: bootconsole [sbi0] disabled
[    0.007400] printk: bootconsole [sbi0] disabled
[    0.009800] Calibrating delay loop (skipped), value calculated using timer frequency.. 0.01 BogoMIPS (lpj=50)
[    0.015600] pid_max: default: 32768 minimum: 301
[    0.030200] random: get_random_bytes called from net_ns_init+0x90/0x328 with crng_init=0
[    0.039200] Mount-cache hash table entries: 1024 (order: 0, 4096 bytes, linear)
[    0.044000] Mountpoint-cache hash table entries: 1024 (order: 0, 4096 bytes, linear)
[    0.107200] ASID allocator disabled (0 bits)
[    0.127000] devtmpfs: initialized
[    0.203200] clocksource: jiffies: mask: 0xffffffff max_cycles: 0xffffffff, max_idle_ns: 19112604462750000 ns
[    0.208200] futex hash table entries: 256 (order: -1, 3072 bytes, linear)
[    0.231600] NET: Registered PF_NETLINK/PF_ROUTE protocol family
[    0.572400] clocksource: Switched to clocksource riscv_clocksource
[    0.662400] NET: Registered PF_INET protocol family
[    0.672200] IP idents hash table entries: 2048 (order: 2, 16384 bytes, linear)
[    0.756800] tcp_listen_portaddr_hash hash table entries: 512 (order: 0, 4096 bytes, linear)
[    0.762800] TCP established hash table entries: 1024 (order: 0, 4096 bytes, linear)
[    0.768600] TCP bind hash table entries: 1024 (order: 0, 4096 bytes, linear)
[    0.775600] TCP: Hash tables configured (established 1024 bind 1024)
[    0.780400] UDP hash table entries: 256 (order: 0, 4096 bytes, linear)
[    0.784000] UDP-Lite hash table entries: 256 (order: 0, 4096 bytes, linear)
[    0.798800] NET: Registered PF_UNIX/PF_LOCAL protocol family
[    0.837200] workingset: timestamp_bits=30 max_order=15 bucket_order=0
[    1.379200] romfs: ROMFS MTD (C) 2007 Red Hat, Inc.
[    1.438400] shaderemu_fb: fb0, pixels at 0x87001000
[    1.514400] [_pi_] rvc sysfs entries created
[    1.516800] shaderemu_gpu: /dev/gpu at 0x86000000, 27648 KiB
[    1.528600] shaderemu_sound: /dev/sound, 3072 KiB of sample memory at 0x87b00000
[    1.777400] phram: test data 1: 16 -> 6d6f722d 2d736631 9370ba0d
[    1.782400] phram: test data 2: 16 -> dcfb438d d72dc4f9 5bcaa98e
[    1.786600] phram: root device: 0xdc00000 at 0x40000000 for erasesize 0x1000
[    1.870800] input: ShaderEmu keyboard as /devices/virtual/input/input0
[    1.911600] input: ShaderEmu pointer as /devices/virtual/input/input1
[    1.926200] rtc-ds1742 3000000.rtc: voltage-low detected.
[    1.956800] rtc-ds1742 3000000.rtc: registered as rtc0
[    1.981200] rtc-ds1742 3000000.rtc: hctosys: unable to read the hardware clock
[    2.021800] NET: Registered PF_PACKET protocol family
[    2.049000] VFS: Mounted root (romfs filesystem) readonly on device 31:0.
[    2.054000] devtmpfs: mounted
[    2.056400] Freeing unused kernel image (initmem) memory: 124K
[    2.060400] Kernel memory protection not selected by kernel config.
[    2.062600] Run /emuinit as init process

> Welcome to userland!
> Setting up overlay mount for writeable root
> Entered overlay chroot, mounting /proc & /sys
> Startup took: 1.82s
                  _ _
                 | (_)
 _ ____   _____  | |_ _ __  _   ___  __
| '__\ \ / / __| | | | '_ \| | | \ \/ /
| |   \ V / (__  | | | | | | |_| |>  <
|_|    \_/ \___| |_|_|_| |_|\__,_/_/\_\ 


                              by _pi_

> Starting login shell on TTY /dev/hvc0
/ # nano-X -p & sleep 2; doom -1 -timedemo demo1; echo LX-''DONE
ShaderEmu display 800x600, GPU drawing
                            DOOM Shareware Startup v1.10                           
Playing demo demo1.lmp.
V_Init: allocate screens.
M_LoadDefaults: Load system defaults.
Z_Init: Init zone memory allocation daemon. 
W_Init: Init WADfiles.
 adding /usr/share/doom1.wad
 couldn't open demo1.lmp
added
===========================================================================
                                Shareware!
===========================================================================
M_Init: Init miscellaneous info.
R_Init: Init DOOM refresh daemon - 
[                   ]..
InitTextures
InitFlats
InitSprites
InitColormaps
R_InitData
R_InitPointToAngle
R_InitTables
R_InitPlanes
R_InitLightTables
R_InitSkyMap
R_InitTranslationsTables
P_Init: Init Playloop state.
I_Init: Setting up machine state.
OPEN GRAPHICS
D_CheckNetGame: Checking network game status.
startskill 2  deathmatch: 0  startmap: 1  startepisode: 1
player 1 of 1 (1 nodes)
S_Init: Setting up sound.
S_Init: default sfx volume 8
I_SetChannels: no sound card
HU_Init: Setting up heads up display.
ST_Init: Init status bar.
doom: 501 frames in 5.0 seconds = 100.0 FPS
doomstat: 8416k/s; a tic 19k (463 tics); a frame 61k draw + 4k show
doomstat: the view 15k a frame and 27k its list (a worker core's), 52 things
doomstat: 2892 sight checks, 2491 answered by a worker's walks (2534 of them)
doom: 610 frames in 5.0 seconds = 122.0 FPS
doomstat: 3185k/s; a tic 19k (610 tics); a frame 5k draw + 0k show
doomstat: the view 2k a frame and 27k its list (a worker core's), 55 things
doomstat: 6131 sight checks, 5182 answered by a worker's walks (5334 of them)
doom: 678 frames in 5.0 seconds = 135.4 FPS
doomstat: 3212k/s; a tic 16k (678 tics); a frame 5k draw + 0k show
doomstat: the view 2k a frame and 24k its list (a worker core's), 51 things
doomstat: 8924 sight checks, 7511 answered by a worker's walks (7589 of them)
doom: 624 frames in 5.0 seconds = 124.7 FPS
doomstat: 3189k/s; a tic 19k (624 tics); a frame 4k draw + 0k show
doomstat: the view 1k a frame and 27k its list (a worker core's), 49 things
doomstat: 11795 sight checks, 9660 answered by a worker's walks (9705 of them)
doom: 565 frames in 5.0 seconds = 112.9 FPS
doomstat: 3261k/s; a tic 23k (565 tics); a frame 4k draw + 0k show
doomstat: the view 1k a frame and 31k its list (a worker core's), 77 things
doomstat: 13659 sight checks, 11119 answered by a worker's walks (11119 of them)
doom: 605 frames in 5.0 seconds = 120.9 FPS
doomstat: 2933k/s; a tic 17k (605 tics); a frame 5k draw + 0k show
doomstat: the view 1k a frame and 35k its list (a worker core's), 106 things
doomstat: 15615 sight checks, 12850 answered by a worker's walks (12597 of them)
doom: 590 frames in 5.0 seconds = 117.2 FPS
doomstat: 3122k/s; a tic 20k (590 tics); a frame 4k draw + 0k show
doomstat: the view 1k a frame and 32k its list (a worker core's), 72 things
doomstat: 17422 sight checks, 14039 answered by a worker's walks (13899 of them)
doom: 712 frames in 5.0 seconds = 142.2 FPS
doomstat: 3084k/s; a tic 15k (712 tics); a frame 4k draw + 0k show
doomstat: the view 1k a frame and 23k its list (a worker core's), 40 things
doomstat: 19308 sight checks, 15752 answered by a worker's walks (15431 of them)
Error: timed 5026 gametics in 1510 realtics
LX-DONE
