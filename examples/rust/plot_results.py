#!/usr/bin/env python3

import os
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
import re
from typing import Dict, List, Tuple

# Global font size
FONT_SIZE = 14

# Monochromatic palette with different patterns
PATTERNS = {
    'solid': '',           # No pattern
    'dots': '....',       # Dotted pattern
    'cross': 'xxxx',      # Cross pattern
    'slash': '////',      # Slash pattern
    'backslash': '\\\\',  # Backslash pattern
    'plus': '++++'        # Plus pattern
}

# Different shades of gray
GRAYS = {
    'dark': '#000000',    # Black
    'medium': '#666666',  # Medium gray
    'light': '#999999'    # Light gray
}

def get_channel_number(folder_name: str) -> int:
    """Extract channel number from folder name."""
    match = re.search(r'channels_(\d+)', folder_name)
    return int(match.group(1)) if match else 0

def read_experiment_data(data_dir: str) -> Tuple[List[int], Dict[str, List[float]]]:
    """Read all experiment data and organize by channel count."""
    channel_numbers = []
    latencies = {
        'p50': [], 'p99': [], 'p999': []
    }
    throughput = {
        'tx': [], 'rx': []
    }
    
    # Get all experiment folders and sort by channel number
    folders = [f for f in os.listdir(data_dir) if os.path.isdir(os.path.join(data_dir, f))]
    folders.sort(key=get_channel_number)
    
    for folder in folders:
        channel_num = get_channel_number(folder)
        if channel_num == 0:
            continue
            
        combined_file = os.path.join(data_dir, folder, f'combined_{channel_num}.csv')
        if not os.path.exists(combined_file):
            continue
            
        # Read the combined data
        df = pd.read_csv(combined_file)
        
        # Store data
        channel_numbers.append(channel_num)
        latencies['p50'].append(df['rtt_p50_us'].iloc[0])
        latencies['p99'].append(df['rtt_p99_us'].iloc[0])
        latencies['p999'].append(df['rtt_p999_us'].iloc[0])
        
        # Convert to million messages per second
        throughput['tx'].append(df['tx_msg_sec'].iloc[0] / 1e6)
        throughput['rx'].append(df['rx_msg_sec'].iloc[0] / 1e6)
    
    return channel_numbers, latencies, throughput

def plot_latencies(channel_numbers: List[int], latencies: Dict[str, List[float]], output_file: str):
    """Create latency plot."""
    plt.figure(figsize=(10, 6))
    
    x = np.arange(len(channel_numbers))
    width = 0.25
    
    # Using different patterns and shades for each percentile
    plt.bar(x - width, latencies['p50'], width, label='P50', 
            color=GRAYS['dark'], hatch=PATTERNS['solid'])
    plt.bar(x, latencies['p99'], width, label='P99', 
            color=GRAYS['medium'], hatch=PATTERNS['dots'])
    plt.bar(x + width, latencies['p999'], width, label='P99.9', 
            color=GRAYS['light'], hatch=PATTERNS['cross'])
    
    plt.xlabel('Number of Channels', fontsize=FONT_SIZE)
    plt.ylabel('Latency (μs)', fontsize=FONT_SIZE)
    plt.title('Latency Percentiles vs Number of Channels', fontsize=FONT_SIZE)
    plt.xticks(x, channel_numbers, fontsize=FONT_SIZE)
    plt.yticks(fontsize=FONT_SIZE)
    plt.legend(fontsize=FONT_SIZE)
    plt.grid(True, linestyle='--', alpha=0.7)
    
    # Add value labels on top of bars (30 degrees counter-clockwise)
    for i in range(len(channel_numbers)):
        # Add small vertical offset to prevent overlap
        offset = max(latencies['p50'][i], latencies['p99'][i], latencies['p999'][i]) * 0.02
        plt.text(i - width, latencies['p50'][i] + offset, f'{latencies["p50"][i]:.1f}', 
                ha='center', va='bottom', rotation=30, fontsize=FONT_SIZE)
        plt.text(i, latencies['p99'][i] + offset, f'{latencies["p99"][i]:.1f}', 
                ha='center', va='bottom', rotation=30, fontsize=FONT_SIZE)
        plt.text(i + width, latencies['p999'][i] + offset, f'{latencies["p999"][i]:.1f}', 
                ha='center', va='bottom', rotation=30, fontsize=FONT_SIZE)
    
    plt.tight_layout()
    plt.savefig(output_file, dpi=300, bbox_inches='tight')
    plt.close()

def plot_throughput(channel_numbers: List[int], throughput: Dict[str, List[float]], output_file: str):
    """Create throughput plot."""
    plt.figure(figsize=(10, 6))
    
    x = np.arange(len(channel_numbers))
    width = 0.35
    
    # Using different patterns and shades for TX/RX
    plt.bar(x - width/2, throughput['tx'], width, label='TX', 
            color=GRAYS['dark'], hatch=PATTERNS['solid'])
    plt.bar(x + width/2, throughput['rx'], width, label='RX', 
            color=GRAYS['medium'], hatch=PATTERNS['dots'])
    
    plt.xlabel('Number of Channels', fontsize=FONT_SIZE)
    plt.ylabel('Throughput (Million msg/sec)', fontsize=FONT_SIZE)
    plt.title('Message Throughput vs Number of Channels', fontsize=FONT_SIZE)
    plt.xticks(x, channel_numbers, fontsize=FONT_SIZE)
    plt.yticks(fontsize=FONT_SIZE)
    plt.legend(fontsize=FONT_SIZE)
    plt.grid(True, linestyle='--', alpha=0.7)
    
    # Add value labels on top of bars (30 degrees counter-clockwise)
    for i in range(len(channel_numbers)):
        # Add small vertical offset to prevent overlap
        offset = max(throughput['tx'][i], throughput['rx'][i]) * 0.02
        plt.text(i - width/2, throughput['tx'][i] + offset, f'{throughput["tx"][i]:.1f}', 
                ha='center', va='bottom', rotation=30, fontsize=FONT_SIZE)
        plt.text(i + width/2, throughput['rx'][i] + offset, f'{throughput["rx"][i]:.1f}', 
                ha='center', va='bottom', rotation=30, fontsize=FONT_SIZE)
    
    plt.tight_layout()
    plt.savefig(output_file, dpi=300, bbox_inches='tight')
    plt.close()

def main():
    # Set style to be more like gnuplot
    plt.style.use('seaborn-v0_8-whitegrid')
    
    # Set global font size
    plt.rcParams.update({'font.size': FONT_SIZE})
    
    # Read data
    data_dir = "data"
    channel_numbers, latencies, throughput = read_experiment_data(data_dir)
    
    # Create plots
    plot_latencies(channel_numbers, latencies, "latency_plot.png")
    plot_throughput(channel_numbers, throughput, "throughput_plot.png")
    
    print("Plots have been generated: latency_plot.png and throughput_plot.png")

if __name__ == "__main__":
    main() 