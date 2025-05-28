#!/usr/bin/env python3

import subprocess
import time
import argparse
import signal
import sys
from typing import List

def parse_args():
    parser = argparse.ArgumentParser(description='Run server instances for channel scalability experiment')
    parser.add_argument('--num-channels', '-n', type=int, required=True,
                      help='Number of channels to test')
    parser.add_argument('--server-ip', type=str, default='10.10.1.1',
                      help='Server IP address')
    parser.add_argument('--msg-size', type=int, default=64,
                      help='Size of messages in bytes')
    parser.add_argument('--msg-window', type=int, default=8,
                      help='Max messages in flight')
    parser.add_argument('--msg-nr', type=int, default=18446744073709551615,
                      help='Number of messages to send')
    parser.add_argument('--verify', action='store_true',
                      help='Verify payload of received messages')
    parser.add_argument('--binary', type=str, default='./target/release/msg_gen',
                      help='Path to server binary')
    return parser.parse_args()

def start_servers(args) -> List[subprocess.Popen]:
    servers = []
    for i in range(args.num_channels):
        port = 1000 + i
        cmd = [
            args.binary,
            '--local-ip', args.server_ip,
            '--local-port', str(port),
            '--msg-size', str(args.msg_size),
            '--msg-window', str(args.msg_window),
            '--msg-nr', str(args.msg_nr)
        ]
        
        if args.verify:
            cmd.append('--verify')
            
        print(f"Starting server on port {port}...")
        server = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        servers.append(server)
        time.sleep(0.1)  # Small delay to ensure servers start properly
    return servers

def main():
    args = parse_args()
    
    # Handle graceful shutdown
    def signal_handler(sig, frame):
        print("\nShutting down servers...")
        for server in servers:
            server.terminate()
        sys.exit(0)
    
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)
    
    # Start servers
    print(f"Starting {args.num_channels} servers on {args.server_ip}...")
    servers = start_servers(args)
    
    print("\nServers are running. Press Ctrl+C to stop.")
    print("Server ports:", [f"{1000 + i}" for i in range(args.num_channels)])
    
    # Keep the script running until interrupted
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nShutting down servers...")
        for server in servers:
            server.terminate()
            server.wait()

if __name__ == '__main__':
    main() 