# In eve console, use the following command to first copy the amba_vert.ko
# out of the container, and then install the kernel module.
# Assuming you've set up the container to have enabled ssh & sftp to
# allow transfer of files from the network into the container

cp $(find / -name "amba_virt.ko" 2>/dev/null | head -n 1) /dev/shm/
insmod /dev/shm/amba_vert.ko
