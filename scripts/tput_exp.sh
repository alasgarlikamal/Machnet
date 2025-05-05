#! /bin/bash
# @brief: do a throughput experiment experiment
#   Assumptions:
#       1. the script will be run on the client machine
#       2. the server has been already deployed.
#       3. the Machnet engine is running on the client machine
# Running the server:
#  ./msg_gen -local_ip 172.17.0.1 -local_port 888

machnet_dir=/home/hawk/dev/machnet
msg_gen="$machnet_dir"/build/src/apps/msg_gen/msg_gen
exp_time=30
output_dir=$HOME/results/machnet/

local_ip=10.0.0.5
remote_ip=10.0.0.7

ssh_user=hawk
ssh_server=10.0.0.6

if [ ! -d "$output_dir" ]; then
	mkdir -p "$output_dir"
fi

setup_server() {
	echo 'about to setup server'
	ssh $ssh_user@$ssh_server << EOF
	cd $machnet_dir
	(bash ./machnet.sh -b --ip \$MACHNET_IP_ADDR --mac \$MACHNET_MAC_ADDR &> /tmp/machnet_engine) &
	sleep 3
	(sudo taskset -c 2 $msg_gen -local_ip $remote_ip -local_port 888 &> /tmp/msg_gen_server) &
	sleep 1
EOF
}

stop_server() {
	ssh $ssh_user@$ssh_server << EOF
	sudo pkill -SIGINT msg_gen
	sudo pkill -SIGINT machnet
EOF
}

generate_traffic() {
	(bash $machnet_dir/machnet.sh -b --ip $MACHNET_IP_ADDR --mac $MACHNET_MAC_ADDR &> /tmp/machnet_engine) &
	sleep 3
	sudo taskset -c 2 "$msg_gen" -local_ip $local_ip -local_port 1234 \
		-remote_ip $remote_ip -remote_port 888 \
		-msg_size "$1" -msg_window "$2"
}

stop_traffic() {
	sudo pkill -SIGINT machnet
	sudo pkill -SIGINT msg_gen
	sleep 1
}

get_output_file_name() {
	echo "$output_dir"/msg_sz_"$1"_wnd_sz_"$2".txt
}

on_signal() {
	stop_server
	stop_traffic
	exit 1
}

trap 'on_signal' SIGINT SIGHUP

# Make sure everything is stopped
stop_server
stop_traffic
sleep 1

# Fixed window size:
msg_size=( 32 64 128 256 512 1024 2048 4096 8192 )
wnd_size=( 512 )

for wnd in "${wnd_size[@]}"; do
	echo Window size: "$wnd"
	for msg_sz in "${msg_size[@]}"; do
		echo Message size: "$msg_sz"
		filename=$(get_output_file_name "$msg_sz" "$wnd")
		setup_server
		(generate_traffic "$msg_sz" "$wnd" &> "$filename" ) &
		sleep $exp_time
		stop_traffic
		stop_server
		sleep 5
	done
done

# Fixed message size:
msg_size=( 8192 )
wnd_size=( 1 2 4 8 16 32 64 128 256 512 1024 )

for wnd in "${wnd_size[@]}"; do
	echo Window size: "$wnd"
	for msg_sz in "${msg_size[@]}"; do
		echo Message size: "$msg_sz"
		filename=$(get_output_file_name "$msg_sz" "$wnd")
		setup_server
		(generate_traffic "$msg_sz" "$wnd" &> "$filename" ) &
		sleep $exp_time
		stop_traffic
		stop_server
		sleep 5
	done
done
