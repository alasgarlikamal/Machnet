#!/usr/bin/env python3

import subprocess
import time
import csv
import os
from datetime import datetime
import re
import argparse
import sys
from typing import List, Dict, Tuple
import numpy as np
import signal
from concurrent.futures import ThreadPoolExecutor, as_completed

def parse_args():
    parser = argparse.ArgumentParser(description='Run client instances for channel scalability experiment')
    parser.add_argument('--num-channels', '-n', type=int, required=True,
                      help='Number of channels to test')
    parser.add_argument('--server-ip', type=str, required=True,
                      help='Server IP address')
    parser.add_argument('--client-ip', type=str, default='10.10.1.2',
                      help='Client IP address')
    parser.add_argument('--duration', '-t', type=int, default=10,
                      help='Duration of each client run in seconds')
    parser.add_argument('--msg-size', type=int, default=64,
                      help='Size of messages in bytes')
    parser.add_argument('--msg-window', type=int, default=8,
                      help='Max messages in flight')
    parser.add_argument('--msg-nr', type=int, default=18446744073709551615,
                      help='Number of messages to send')
    parser.add_argument('--verify', action='store_true',
                      help='Verify payload of received messages')
    parser.add_argument('--binary', type=str, default='./target/release/msg_gen',
                      help='Path to client binary')
    return parser.parse_args()

def parse_client_output(output: str) -> Dict:
    # Parse the output line like: [2025-05-28T20:44:43Z INFO  msg_gen] TX/RX (msg/sec, Gbps): (751.4K/751.4K, 0.385/0.385). RTT (p50/99/99.9 us): 9/26/31
    pattern = r'TX/RX \(msg/sec, Gbps\): \((\d+\.?\d*[KMG]?)/(\d+\.?\d*[KMG]?),\s*(\d+\.?\d*)/(\d+\.?\d*)\).*RTT \(p50/99/99.9 us\): (\d+)/(\d+)/(\d+)'
    match = re.search(pattern, output)
    
    if not match:
        raise ValueError(f"Could not parse client output: {output}")
    
    tx_msg, rx_msg, tx_gbps, rx_gbps, rtt_p50, rtt_p99, rtt_p999 = match.groups()
    
    # Convert K/M/G suffixes to actual numbers
    def convert_to_number(s):
        multipliers = {'K': 1000, 'M': 1000000, 'G': 1000000000}
        for suffix, mult in multipliers.items():
            if suffix in s:
                return float(s.replace(suffix, '')) * mult
        return float(s)
    
    return {
        'tx_msg_sec': convert_to_number(tx_msg),
        'rx_msg_sec': convert_to_number(rx_msg),
        'tx_gbps': float(tx_gbps),
        'rx_gbps': float(rx_gbps),
        'rtt_p50_us': float(rtt_p50),
        'rtt_p99_us': float(rtt_p99),
        'rtt_p999_us': float(rtt_p999)
    }

def run_client(client_id: int, args) -> Tuple[int, Dict]:
    port = 1000 + client_id
    cmd = [
        args.binary,
        '--local-ip', args.client_ip,
        '--local-port', str(port),
        '--remote-ip', args.server_ip,
        '--remote-port', str(port),
        '--msg-size', str(args.msg_size),
        '--msg-window', str(args.msg_window),
        '--msg-nr', str(args.msg_nr)
    ]
    
    if args.verify:
        cmd.append('--verify')
    
    print(f"Starting client {client_id + 1}/{args.num_channels}...")
    
    try:
        # Start the process
        process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,  # Line buffered
            universal_newlines=True
        )
        
        # Wait for the specified duration
        try:
            process.wait(timeout=args.duration)
        except subprocess.TimeoutExpired:
            # Send SIGINT for graceful shutdown
            process.send_signal(signal.SIGINT)
            try:
                # Give it a moment to clean up
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                # Force kill if still running
                process.kill()
                process.wait()
        
        # Get the output
        stdout, stderr = process.communicate()
        
        # Combine stdout and stderr since the client logs to stderr
        combined_output = stdout + stderr
        
        if process.returncode != 0 and process.returncode != -2 and process.returncode != -9:  # -2 is SIGINT and -9 is SIGKILL
            raise RuntimeError(f"Client {client_id} failed with error: {stderr}")
        
        return client_id, parse_client_output(combined_output)
        
    except Exception as e:
        # Ensure we capture output even if an exception occurs
        stdout, stderr = process.communicate() if 'process' in locals() else ('', '')
        raise e

def write_csv(data: Dict, filename: str):
    with open(filename, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=data.keys())
        writer.writeheader()
        writer.writerow(data)

def main():
    args = parse_args()
    timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
    
    # Run clients in parallel using ThreadPoolExecutor
    all_data = []
    with ThreadPoolExecutor(max_workers=args.num_channels) as executor:
        # Submit all client tasks
        future_to_client = {
            executor.submit(run_client, i, args): i 
            for i in range(args.num_channels)
        }
        
        # Process results as they complete
        for future in as_completed(future_to_client):
            client_id = future_to_client[future]
            try:
                _, data = future.result()
                print(f"Client {client_id + 1} completed successfully")
                all_data.append(data)
                
                # Write individual client data
                filename = f"client_{timestamp}_{client_id + 1}.csv"
                write_csv(data, filename)
                print(f"Wrote data to {filename}")
                
            except Exception as e:
                print(f"Client {client_id + 1} failed: {e}")
    
    if not all_data:
        print("No successful client runs!")
        sys.exit(1)
    
    # Create combined data
    combined_data = {
        'tx_msg_sec': sum(d['tx_msg_sec'] for d in all_data),
        'rx_msg_sec': sum(d['rx_msg_sec'] for d in all_data),
        'tx_gbps': sum(d['tx_gbps'] for d in all_data),
        'rx_gbps': sum(d['rx_gbps'] for d in all_data),
        'rtt_p50_us': np.mean([d['rtt_p50_us'] for d in all_data]),
        'rtt_p99_us': np.mean([d['rtt_p99_us'] for d in all_data]),
        'rtt_p999_us': np.mean([d['rtt_p999_us'] for d in all_data])
    }
    
    # Write combined data
    combined_filename = f"client_{timestamp}_combined_{args.num_channels}.csv"
    write_csv(combined_data, combined_filename)
    print(f"Wrote combined data to {combined_filename}")

if __name__ == '__main__':
    main() 