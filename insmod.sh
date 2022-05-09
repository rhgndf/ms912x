make clean
make all -j
sudo rmmod ms912x
sudo insmod ms912x.ko
