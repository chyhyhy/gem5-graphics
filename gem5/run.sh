gem5="build/ARM/gem5.opt" #"build/ARM/gem5.debug" 

if [ $1 == "c" ] || [ $1 == "a" ];
then
python2 /usr/bin/scons build/ARM/gem5.opt EXTRAS=../gem5-gpu/src:../gpgpu-sim -j8
fi

# os_type="linux"
os_type="android-nougat"

config="arm_linux_config.py"
machine_type="VExpress_GEM5_V1"

#linux
# disk_img="/root/imgs/binaries/rootfs.img"
# kernel_img="/root/imgs/binaries/vmlinux"

#android
disk_img="/root/imgs/binaries/android_root.img"
kernel_img="/root/imgs/binaries/vmlinux_gem5"

# cpu
cpu_type="AtomicSimpleCPU" #"O3_ARM_v7a_3" #

# gpu
g_depth_shader=1
gpgpusim_config="Fermi"
gpu_core_config="Fermi"

# memory
mem_size="1024MB"

#ckeckpoint
usecheckpoint=0
checkpoint_dir="/root/emerald/gem5-graphics/gem5/m5out/"
checkpoint=1


if [ $1 == "r" ] || [ $1 == "a" ]
then
    if [ $usecheckpoint == 1 ]
    then
    $gem5 \
    ../gem5-gpu/configs/$config \
    --cpu-type $cpu_type \
    --disk-image $disk_img \
    --machine-type $machine_type \
    --gpgpusim_config $gpgpusim_config \
    --gpu_core_config $gpu_core_config \
    --g_depth_shader $g_depth_shader \
    --os-type $os_type \
    --kernel $kernel_img \
    --mem-size $mem_size \
    --checkpoint-restore $checkpoint \
    --checkpoint-dir $checkpoint_dir \
    # --caches \
    # --l2cache \
    else
    $gem5 \
    ../gem5-gpu/configs/$config \
    --cpu-type $cpu_type \
    --disk-image $disk_img \
    --machine-type $machine_type \
    --gpgpusim_config $gpgpusim_config \
    --gpu_core_config $gpu_core_config \
    --g_depth_shader $g_depth_shader \
    --os-type $os_type \
    --kernel $kernel_img \
    --mem-size $mem_size \
    # --caches \
    # --l2cache \
    fi
fi