#!/bin/bash

# Install dependancies
sudo apt update
sudo apt install -y build-essential automake texinfo

git clone https://github.com/fshahinfar1/netperf
cd ./netperf
git checkout 0d871abca4d2e5ac569148edddc618b4832a2c1a
./autogen.sh
./configure
make

# Configure Machines

# On Azure:
# Configure route between experiment NICs
# On machine 0: ip route add 10.0.0.7/32 via 10.0.0.5 dev eth1
# On machine 1: ip route add 10.0.0.5/32 via 10.0.0.7 dev eth1
# ping machine to make sure things work


# Do experiment

# Machine 1: Server
./netserver -4 -L 10.0.0.7 -D

# Machine 0: Client

# -l: test duration

# -r: client,server message size
# -K: CC algorithm
# -D: disable nagle's algo
# -s: local send buffer size
# -S: remote send buffer size

# message size 64 B
./netperf -H 10.0.0.7 -t TCP_RR -l 30 -- -o min_latency,max_latency,mean_latency,P50_LATENCY,P99_LATENCY,P99_9_LATENCY -D L,R -K cubic,cubic -r 64,64 -b 1 -s 65536 -S 65536
# message size 32 KB
./netperf -H 10.0.0.7 -t TCP_RR -l 30 -- -o min_latency,max_latency,mean_latency,P50_LATENCY,P99_LATENCY,P99_9_LATENCY -D L,R -K cubic,cubic -r 32768,32768 -b 1 -s 65536 -S 65536

