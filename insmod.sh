make clean
make all -j
sudo rmmod ms912x 2>/dev/null
sudo insmod ms912x.ko
