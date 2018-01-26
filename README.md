# utils

## mpi_dec.c -rockchip video acceleration testing utility
* architecture
	* using MPP/MPI API to accelerate video decoding
		* https://github.com/rockchip-linux/mpp
		* https://github.com/rockchip-linux/libdrm-rockchip
		* for output buffers using [pure external mode](https://github.com/rockchip-linux/mpp/blob/release_20170616/readme.txt#L354) with with zero copy [DRM buffers](https://github.com/rockchip-linux/mpp/blob/release_20170616/inc/mpp_buffer.h#L152), [commit flow group](https://github.com/rockchip-linux/mpp/blob/release_20170616/inc/mpp_buffer.h#L97)
	* running in asynchronous split standard mode with threads
		* main() thread - init/deinit and feed data in READ_BUF_SIZE (1MB) buffers
		* frame_thread() thread - setup DRM buffers (based on video size) and pick+free decoded buffers
		* display_thread() thread - display rendered DRM buffer to plane 
	* running in maximum speed and display ASAP (no PTS wait)
* prerequisites
	* tested on RK3328/ROCK64 only
	* minimal os without X11/Wayland GUI (renders directly to framebuffer)
	* Rockchip linux-4.4 with some patches like https://github.com/mcerveny/rockchip-linux/tree/release-4.4-hdr
	* Rockchip MPP and LIBDRM like https://launchpad.net/~ayufan/+archive/ubuntu/rock64-ppa
* compiling
	* gcc -o mpi_dec -I/usr/include/rockchip -lrockchip_mpp -I/usr/include/libdrm -ldrm -lpthread -g -Wno-multichar mpi_dec.c
* testing
	* need raw video sources
```
# ffmpeg -i "LG Chess 4K Demo.mp4" -c:v copy -an -bsf:v hevc_mp4toannexb 2k60hdr.hevc
# ffmpeg -i bbb-3840x2160-cfg02.mkv -c:v copy -an -bsf:v hevc_mp4toannexb 2k60.hevc
# ffmpeg -i bbb_sunflower_2160p_60fps_normal.mp4 -c:v -an -bsf:v h264_mp4toannexb 4k60.h264
```
*	* sometimes speedup CPU and/or DDR is needed
```
# cat /sys/devices/system/cpu/cpufreq/policy0/scaling_available_frequencies 
408000 600000 816000 1008000 1200000 1296000 
# cat /sys/class/devfreq/dmc/available_frequencies 
400000000 600000000 786000000 800000000 850000000 933000000 1066000000
# echo 1008000 > /sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq
# echo 933000000 > /sys/class/devfreq/dmc/min_freq 
```
*	* test formats supported by Rockchip CPU
```
# ./mpi_dec
usage: ./mpi_dec raw_filename mpp_coding_id [nodisplay]

mpi: mpp coding type support list:
mpi: type: dec id 0 coding: mpeg2            id 2
mpi: type: dec id 0 coding: mpeg4            id 4
mpi: type: dec id 0 coding: h.263            id 3
mpi: type: dec id 0 coding: h.264/AVC        id 7
mpi: type: dec id 0 coding: h.265/HEVC       id 16777220
mpi: type: dec id 0 coding: vp8              id 9
mpi: type: dec id 0 coding: VP9              id 10
mpi: type: dec id 0 coding: avs+             id 16777221
mpi: type: dec id 0 coding: jpeg             id 8
mpi: type: enc id 1 coding: h.264/AVC        id 7
mpi: type: enc id 1 coding: jpeg             id 8
```
*	* test decode+display
```
# ./mpi_dec ../4k60hdr.hevc 16777220

# ## check clocks
# egrep 'armclk |aclk_rkvdec |cabac |vdec_core |ddrc ' /sys/kernel/debug/clk/clk_summary 
             armclk                       0            0  1008000000          0 0  
          sclk_vdec_core                  2            3   300000000          0 0  
             aclk_rkvdec                  3            4   600000000          0 0  
          sclk_vdec_cabac                 2            3   400000000          0 0  
          sclk_ddrc                       2            2   924000000          0 0  

# ## check power activated accelerator
# cat ./kernel/debug/pm_genpd/pm_genpd_summary 
domain                          status          slaves
    /device                                             runtime status
----------------------------------------------------------------------
pd_vpu                          on              
    /devices/platform/ff350800.iommu                    suspended
    /devices/platform/vpu_combo                         active
pd_video                        on              
    /devices/platform/ff360480.iommu                    active
    /devices/platform/ff360000.rkvdec                   active
pd_hevc                         on              
    /devices/platform/ff330200.iommu                    suspended
    /devices/platform/ff340800.iommu                    suspended
    /devices/platform/ff330000.h265e                    unsupported
    /devices/platform/ff340000.vepu                     unsupported

# ## check output chain
# cat /sys/kernel/debug/dri/0/summary
VOP [ff370000.vop]: ACTIVE
    Connector: HDMI-A
	overlay_mode[1] bus_format[2016] output_mode[f] color_space[10]
    Display mode: 3840x2160p30
	clk[297000] real_clk[297000] type[40] flag[5]
	H: 3840 4016 4104 4400
	V: 2160 2168 2178 2250
    win0-0: ACTIVE
	format: NA12 little-endian (0x3231414e) HDR[2] color_space[10]
	csc: y2r[0] r2r[0] r2y[0] csc mode[3]
	zpos: 0
	src: pos[0x0] rect[3840x2160]
	dst: pos[0x0] rect[3840x2160]
	buf[0]: addr: 0x0000000011acf000 pitch: 4864 offset: 0
	buf[1]: addr: 0x0000000011acf000 pitch: 4864 offset: 10506240
    win1-0: DISABLED
    win2-0: DISABLED
    post: sdr2hdr[0] hdr2sdr[0]
    pre : sdr2hdr[0]
    post CSC: r2y[0] y2r[0] CSC mode[3]

# ## check hdmi output
# cat /sys/kernel/debug/dw-hdmi/status
PHY: enabled			Mode: HDMI
Pixel Clk: 297000000Hz		TMDS Clk: 297000000Hz
Color Format: YUV422		Color Depth: 10 bit
Colorimetry: ITU.BT2020		EOTF: ST2084
x0: 35400				y0: 14599
x1: 8500				y1: 39850
x2: 6550				y2: 2300
white x: 15635			white y: 16451
max lum: 0			min lum: 0
max cll: 0			max fall: 0
```

