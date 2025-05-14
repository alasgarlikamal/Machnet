#! /bin/bash

# set -x

# Assumptions:
#  - There are many hard coded values
#  - Make sure machines can ssh
#  - Install dirs are the same on both machines

# results directory
outdir=$HOME/results/

# ssh config
ssh_user=farbod
server=138.37.32.108

# server client addresses
server_ip="10.10.0.1"
server_mac="e8:eb:d3:a7:0c:b6"
client_ip="10.10.0.2"
client_mac="b8:ce:f6:d2:12:c6"

# install dir
curdir=$(dirname $0)
machnet_install_dir="/home/farbod/machnet"
msg_gen=$machnet_install_dir/build/src/apps/msg_gen/msg_gen
machnet_sh=$machnet_install_dir/machnet.sh

# experiment parameters
count_threads=1 # will be changed in the main
base_port=900
msg_size=64
msg_window=1024
exp_duration=25

stop_everthing() {
	sudo pkill -INT msg_gen
	sudo pkill -INT machnet
	ssh $ssh_user@$server << EOF
	sudo pkill -INT msg_gen
	sudo pkill -INT machnet
EOF
}

on_signal() {
	stop_everthing
	exit 0
}
trap 'on_signal' SIGINT SIGHUP


# Do one experiment with given number of engine threads
one_round() {
	stop_everthing

	# NOTE: server's nic is on numa node 1, allocate cpu from odd cores
	# setup servers
	ssh $ssh_user@$server > /dev/null << EOF
		cd $machnet_install_dir
		(nohup bash ./machnet.sh -b --mac $server_mac --ip $server_ip -e $count_threads &> /tmp/machnet_stdout.txt) &
		sleep 5
		base_port=$base_port
		for i in \$(seq $count_threads); do
			port=\$((base_port + i))
			tmp_core=\$((11 + \$i * 2))
			(nohup taskset -c \$tmp_core $msg_gen --local_ip $server_ip --local_port \$port &> /tmp/msg_gen_\$i.txt) &
			sleep 1
		done
EOF

	sleep 5

	# setup clients
	cd $machnet_install_dir
	(nohup bash ./machnet.sh -b --mac $client_mac --ip $client_ip -e $count_threads &> /tmp/machnet_stdout.txt) &
	sleep 5
	# client's NIC is on numa node 0 allocate from even cores
	for i in $(seq $count_threads); do
		port=$((base_port + i))
		tmp_core=$((10 + $i * 2))
		(nohup taskset -c $tmp_core $msg_gen --local_ip $client_ip --local_port $port \
			--remote_ip $server_ip --remote_port $port \
			--msg_window $msg_window --msg_size $msg_size &> /tmp/msg_gen_$i.txt) &
		done

	# experiment duration
	sleep $exp_duration

	stop_everthing
}

main() {
	list=( 1 2 3 4 5 )
	# list=( 5 )
	for i in ${list[@]}; do
		echo number of engines: $i
		count_threads=$i
		one_round
		sleep 2
		dir=$outdir/engines_$i/
		mkdir -p $dir
		mv /tmp/msg_gen_*.txt $dir
		mv /tmp/machnet_stdout.txt $dir
		scp $ssh_user@$server:/tmp/machnet_stdout.txt $dir/server_machnet_stdout.txt
		scp $ssh_user@$server:/tmp/msg_gen_1.txt $dir/server_msg_gen_1.txt
		sleep 2
	done
}

# start from main function
main
