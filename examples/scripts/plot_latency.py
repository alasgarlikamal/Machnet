#!/usr/bin/env python3

import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
import argparse
import sys
from pathlib import Path

def setup_gnuplot_style():
    """Configure matplotlib to look like gnuplot"""
    plt.style.use('classic')  # Use classic style as base
    
    # Set gnuplot-like parameters
    plt.rcParams.update({
        'font.size': 12,
        'font.family': 'serif',
        'axes.linewidth': 1.5,
        'axes.grid': True,
        'grid.linewidth': 0.5,
        'grid.alpha': 0.3,
        'lines.linewidth': 2,
        'lines.markersize': 6,
        'xtick.direction': 'in',
        'ytick.direction': 'in',
        'xtick.major.size': 6,
        'ytick.major.size': 6,
        'xtick.minor.size': 3,
        'ytick.minor.size': 3,
        'legend.frameon': True,
        'legend.fancybox': False,
        'legend.shadow': False,
        'legend.framealpha': 1.0,
        'legend.edgecolor': 'black',
        'figure.facecolor': 'white',
        'axes.facecolor': 'white'
    })

def plot_all_latencies(df, output_dir, output_prefix="latency_plot"):
    """Plot all latency metrics on a single figure"""
    setup_gnuplot_style()
    
    fig, ax = plt.subplots(1, 1, figsize=(12, 8))
    
    # Define colors and line styles (gnuplot-like)
    colors = ['#e41a1c', '#377eb8', '#4daf4a', '#984ea3', '#ff7f00', '#ffff33', '#a65628', '#f781bf']
    markers = ['o', 's', '^', 'v', 'd', '<', '>', 'p']
    
    # Plot corrected latencies (solid lines)
    ax.plot(df['rate'], df['corrected_avg'], color=colors[0], linestyle='-', 
            marker=markers[0], label='Corrected Average', markerfacecolor='white', markeredgecolor=colors[0])
    ax.plot(df['rate'], df['corrected_p50'], color=colors[1], linestyle='-', 
            marker=markers[1], label='Corrected P50', markerfacecolor='white', markeredgecolor=colors[1])
    ax.plot(df['rate'], df['corrected_p99'], color=colors[2], linestyle='-', 
            marker=markers[2], label='Corrected P99', markerfacecolor='white', markeredgecolor=colors[2])
    ax.plot(df['rate'], df['corrected_p99_9'], color=colors[3], linestyle='-', 
            marker=markers[3], label='Corrected P99.9', markerfacecolor='white', markeredgecolor=colors[3])
    
    # Plot uncorrected latencies (dashed lines)
    ax.plot(df['rate'], df['uncorrected_avg'], color=colors[0], linestyle='--', 
            marker=markers[4], label='Uncorrected Average', markerfacecolor='white', markeredgecolor=colors[0])
    ax.plot(df['rate'], df['uncorrected_p50'], color=colors[1], linestyle='--', 
            marker=markers[5], label='Uncorrected P50', markerfacecolor='white', markeredgecolor=colors[1])
    ax.plot(df['rate'], df['uncorrected_p99'], color=colors[2], linestyle='--', 
            marker=markers[6], label='Uncorrected P99', markerfacecolor='white', markeredgecolor=colors[2])
    ax.plot(df['rate'], df['uncorrected_p99_9'], color=colors[3], linestyle='--', 
            marker=markers[7], label='Uncorrected P99.9', markerfacecolor='white', markeredgecolor=colors[3])
    
    ax.set_xlabel('Load (requests/sec)')
    ax.set_ylabel('Latency (μs)')
    ax.set_title('Load-Latency (All Percentiles)')
    ax.legend(bbox_to_anchor=(1.05, 1), loc='upper left')
    ax.grid(True, alpha=0.3)
    
    # Set origin at (0,0) and auto limits
    ax.set_xlim(left=0)
    ax.set_ylim([0,200])
    # ax.autoscale()
    
    plt.tight_layout()
    
    # Save the plot
    output_file = output_dir / f"{output_prefix}_all.png"
    plt.savefig(output_file, dpi=300, bbox_inches='tight')
    print(f"Saved plot: {output_file}")
    
    plt.show()

def plot_single_percentile(df, percentile, output_dir, output_prefix="latency_plot"):
    """Plot a specific percentile for corrected and uncorrected measurements"""
    setup_gnuplot_style()
    
    fig, ax = plt.subplots(1, 1, figsize=(10, 6))
    
    # Define colors
    colors = ['#e41a1c', '#377eb8']
    markers = ['o', 's']
    
    # Map percentile to column names
    if percentile == 50:
        corrected_col = 'corrected_p50'
        uncorrected_col = 'uncorrected_p50'
        title_suffix = 'P50 (Median)'
    elif percentile == 99:
        corrected_col = 'corrected_p99'
        uncorrected_col = 'uncorrected_p99'
        title_suffix = 'P99'
    elif percentile == 99.9:
        corrected_col = 'corrected_p99_9'
        uncorrected_col = 'uncorrected_p99_9'
        title_suffix = 'P99.9'
    else:
        # Handle average case
        corrected_col = 'corrected_avg'
        uncorrected_col = 'uncorrected_avg'
        title_suffix = 'Average'
    
    # Plot corrected and uncorrected
    ax.plot(df['rate'], df[corrected_col], color=colors[0], linestyle='-', 
            marker=markers[0], label=f'Corrected {title_suffix}', 
            markerfacecolor='white', markeredgecolor=colors[0])
    ax.plot(df['rate'], df[uncorrected_col], color=colors[1], linestyle='--', 
            marker=markers[1], label=f'Uncorrected {title_suffix}', 
            markerfacecolor='white', markeredgecolor=colors[1])
    
    ax.set_xlabel('Load (requests/sec)')
    ax.set_ylabel('Latency (μs)')
    ax.set_title(f'{title_suffix} Latency vs Load')
    ax.legend()
    ax.grid(True, alpha=0.3)
    
    # Set origin at (0,0) and auto limits
    # ax.set_xlim(left=0)
    # ax.set_ylim(bottom=0)
    ax.set_xlim([0,100000])
    ax.set_ylim([0,100])
    # ax.autoscale()  
    
    plt.tight_layout()
    
    # Save the plot
    percentile_str = str(percentile).replace('.', '_')
    output_file = output_dir / f"{output_prefix}_p{percentile_str}.png"
    plt.savefig(output_file, dpi=300, bbox_inches='tight')
    print(f"Saved plot: {output_file}")
    
    plt.show()

def plot_combined_latency(df, output_dir, output_prefix="latency_plot"):
    """Plot all latency percentiles on a single chart"""
    setup_gnuplot_style()
    
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(16, 6))
    fig.suptitle('Latency Percentiles vs Load', fontsize=16, fontweight='bold')
    
    # Colors and styles
    colors = ['#e41a1c', '#377eb8', '#4daf4a', '#984ea3']
    markers = ['o', 's', '^', 'v']
    
    # Corrected latencies
    ax1.plot(df['rate'], df['corrected_avg'], color=colors[0], marker=markers[0], 
             label='Average', markerfacecolor='white', markeredgecolor=colors[0])
    ax1.plot(df['rate'], df['corrected_p50'], color=colors[1], marker=markers[1], 
             label='P50', markerfacecolor='white', markeredgecolor=colors[1])
    ax1.plot(df['rate'], df['corrected_p99'], color=colors[2], marker=markers[2], 
             label='P99', markerfacecolor='white', markeredgecolor=colors[2])
    ax1.plot(df['rate'], df['corrected_p99_9'], color=colors[3], marker=markers[3], 
             label='P99.9', markerfacecolor='white', markeredgecolor=colors[3])
    
    ax1.set_xlabel('Load (requests/sec)')
    ax1.set_ylabel('Latency (μs)')
    ax1.set_title('Corrected Latency Measurements')
    ax1.legend()
    ax1.grid(True, alpha=0.3)
    
    # Set origin at (0,0) and auto limits for ax1
    ax1.set_xlim(left=0)
    ax1.set_ylim(bottom=0)
    ax1.autoscale()
    
    # Uncorrected latencies
    ax2.plot(df['rate'], df['uncorrected_avg'], color=colors[0], marker=markers[0], 
             label='Average', markerfacecolor='white', markeredgecolor=colors[0])
    ax2.plot(df['rate'], df['uncorrected_p50'], color=colors[1], marker=markers[1], 
             label='P50', markerfacecolor='white', markeredgecolor=colors[1])
    ax2.plot(df['rate'], df['uncorrected_p99'], color=colors[2], marker=markers[2], 
             label='P99', markerfacecolor='white', markeredgecolor=colors[2])
    ax2.plot(df['rate'], df['uncorrected_p99_9'], color=colors[3], marker=markers[3], 
             label='P99.9', markerfacecolor='white', markeredgecolor=colors[3])
    
    ax2.set_xlabel('Load (requests/sec)')
    ax2.set_ylabel('Latency (μs)')
    ax2.set_title('Uncorrected Latency Measurements')
    ax2.legend()
    ax2.grid(True, alpha=0.3)
    
    # Set origin at (0,0) and auto limits for ax2
    ax2.set_xlim(left=0)
    ax2.set_ylim(bottom=0)
    ax2.autoscale()
    
    plt.tight_layout()
    
    # Save the plot
    output_file = output_dir / f"{output_prefix}_combined.png"
    plt.savefig(output_file, dpi=300, bbox_inches='tight')
    print(f"Saved plot: {output_file}")
    
    plt.show()

def plot_throughput_vs_target(df, output_dir, output_prefix="latency_plot"):
    """Plot actual throughput vs target rate"""
    setup_gnuplot_style()
    
    fig, ax = plt.subplots(1, 1, figsize=(10, 6))
    
    # Plot actual vs target
    ax.plot(df['rate'], df['actual_req_sec'], 'o-', color='#e41a1c', 
            label='Actual Throughput', markerfacecolor='white', markeredgecolor='#e41a1c')
    ax.plot(df['rate'], df['rate'], '--', color='#377eb8', 
            label='Target Rate', alpha=0.7)
    
    # Calculate and show efficiency
    efficiency = (df['actual_req_sec'] / df['rate'] * 100).round(1)
    
    ax.set_xlabel('Target Rate (requests/sec)')
    ax.set_ylabel('Actual Throughput (requests/sec)')
    ax.set_title('Throughput: Actual vs Target')
    ax.legend()
    ax.grid(True, alpha=0.3)
    
    # Set origin at (0,0) and auto limits
    ax.set_xlim(left=0)
    ax.set_ylim(bottom=0)
    ax.autoscale()
    
    # Add efficiency annotations for some points
    for i in range(0, len(df), max(1, len(df)//5)):  # Show efficiency for ~5 points
        ax.annotate(f'{efficiency.iloc[i]}%', 
                   (df['rate'].iloc[i], df['actual_req_sec'].iloc[i]),
                   xytext=(5, 5), textcoords='offset points', fontsize=9)
    
    plt.tight_layout()
    
    # Save the plot
    output_file = output_dir / f"{output_prefix}_throughput.png"
    plt.savefig(output_file, dpi=300, bbox_inches='tight')
    print(f"Saved plot: {output_file}")
    
    plt.show()

def main():
    parser = argparse.ArgumentParser(description='Plot latency curves from experiment results')
    parser.add_argument('csv_file', help='CSV file with experiment results')
    parser.add_argument('output_dir', help='Output directory for plots')
    parser.add_argument('-o', '--output', default='latency_plot', 
                        help='Output file prefix (default: latency_plot)')
    parser.add_argument('-p', '--percentile', type=float, choices=[50, 99, 99.9], 
                        help='Plot specific percentile only (50, 99, or 99.9)')
    parser.add_argument('--combined', action='store_true', 
                        help='Plot all percentiles on combined charts (deprecated)')
    parser.add_argument('--throughput', action='store_true',
                        help='Plot throughput comparison')
    parser.add_argument('--all', action='store_true',
                        help='Plot all percentiles (corrected and uncorrected) in single figure')
    
    args = parser.parse_args()
    
    # Create output directory
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    
    # Check if file exists
    csv_path = Path(args.csv_file)
    if not csv_path.exists():
        print(f"Error: File {args.csv_file} not found")
        sys.exit(1)
    
    # Read the data
    try:
        df = pd.read_csv(csv_path)
        print(f"Loaded {len(df)} data points from {args.csv_file}")
    except Exception as e:
        print(f"Error reading CSV file: {e}")
        sys.exit(1)
    
    # Filter successful experiments only
    df_success = df[df['success'] == True].copy()
    if len(df_success) == 0:
        print("No successful experiments found in the data")
        sys.exit(1)
    
    print(f"Using {len(df_success)} successful experiments")
    
    # Generate plots based on arguments
    if args.percentile is not None:
        plot_single_percentile(df_success, args.percentile, output_dir, args.output)
    elif args.all:
        plot_all_latencies(df_success, output_dir, args.output)
    elif args.combined:
        print("Warning: --combined is deprecated, use --all instead")
        plot_combined_latency(df_success, output_dir, args.output)
    elif args.throughput:
        plot_throughput_vs_target(df_success, output_dir, args.output)
    else:
        # Default: plot all latencies in single figure
        plot_all_latencies(df_success, output_dir, args.output)
    
    # Print summary statistics
    print("\nSummary Statistics:")
    print(f"Rate range: {df_success['rate'].min():.0f} - {df_success['rate'].max():.0f} req/sec")
    print(f"Corrected latency range:")
    print(f"  Average: {df_success['corrected_avg'].min():.1f} - {df_success['corrected_avg'].max():.1f} μs")
    print(f"  P99: {df_success['corrected_p99'].min():.1f} - {df_success['corrected_p99'].max():.1f} μs")
    print(f"  P99.9: {df_success['corrected_p99_9'].min():.1f} - {df_success['corrected_p99_9'].max():.1f} μs")

if __name__ == "__main__":
    main() 