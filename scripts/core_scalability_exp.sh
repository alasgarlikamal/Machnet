#! /bin/bash

set -x

# Assumptions:
#  - There are many hard coded values
#  - Make sure machines can ssh
#  - Install dirs are the same on both machines

# results directory
outdir=$HOME/results/

# ssh config
ssh_user=farbod
server=128.110.218.96

# server client addresses
server_ip="10.10.0.1"
server_mac="9c:dc:71:5c:ef:d1"
client_ip="10.10.0.2"
client_mac="9c:dc:71:5d:51:71"

# install dir
curdir=$(dirname $0)
machnet_install_dir="/users/farbod/dev/machnet"
msg_gen=$machnet_install_dir/build/src/apps/msg_gen/msg_gen
machnet_sh=$machnet_install_dir/machnet.sh

# experiment parameters
count_threads=1 # will be changed in the main
base_port=900
msg_size=64
msg_window=1024
exp_duration=15

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

	# setup servers
	ssh $ssh_user@$server << EOF
		cd $machnet_install_dir
		(nohup bash ./machnet.sh -b --mac $server_mac --ip $server_ip -e $count_threads &> /tmp/machnet_stdout.txt) &
		sleep 5
		base_port=$base_port
		for i in \$(seq $count_threads); do
			port=\$((base_port + i))
			(nohup $msg_gen --local_ip $server_ip --local_port \$port &> /tmp/msg_gen_\$i.txt) &
			sleep 1
		done
EOF

	sleep 5

	# setup clients
	cd $machnet_install_dir
	(nohup bash ./machnet.sh -b --mac $client_mac --ip $client_ip -e $count_threads &> /tmp/machnet_stdout.txt) &
	sleep 5
	for i in $(seq $count_threads); do
		port=$((base_port + i))
		(nohup $msg_gen --local_ip $client_ip --local_port $port \
			--remote_ip $server_ip --remote_port $port \
			--msg_window $msg_window --msg_size $msg_size &> /tmp/msg_gen_$i.txt) &
		done

	# experiment duration
	sleep $exp_duration

	stop_everthing
}

main() {
	for i in 1 2 3 4; do
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
