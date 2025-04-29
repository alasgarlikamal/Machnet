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
exp_time=25
output_dir=$HOME/results/

local_ip=10.0.0.5
remote_ip=10.0.0.7

if [ ! -d "$output_dir" ]; then
	mkdir -p "$output_dir"
fi

generate_traffic() {
	sudo taskset -c 5 "$msg_gen" -local_ip $local_ip -local_port 1234 \
		-remote_ip $remote_ip -remote_port 888 \
		-msg_size "$1" -msg_window "$2"
}

stop_traffic() {
	sudo pkill -SIGINT msg_gen
	sleep 1
}

get_output_file_name() {
	echo "$output_dir"/msgsz_"$1"_wndsz_"$2".txt
}

# Fixed window size:
# msg_size=( 64 128 256 512 1024 2048 4096 8192 )
# wnd_size=( 512 )

# Fixed message size:
msg_size=( 8192 )
wnd_size=( 1 2 4 8 16 32 64 128 256 512 1024 )

for wnd in "${wnd_size[@]}"; do
	echo Window size: "$wnd"
	for msg_sz in "${msg_size[@]}"; do
		echo Message size: "$msg_sz"
		filename=$(get_output_file_name "$msg_sz" "$wnd")
		(generate_traffic "$msg_sz" "$wnd" &> "$filename" ) &
		sleep $exp_time
		stop_traffic
		sleep 5
	done
done
