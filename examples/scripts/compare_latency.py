#!/usr/bin/env python3

import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
import argparse
import sys
from pathlib import Path
import glob

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

def find_csv_file(directory):
    """Find the experiment results CSV file in a directory"""
    csv_pattern = directory / "experiment_results_*.csv"
    csv_files = list(Path(directory).glob("experiment_results_*.csv"))
    
    if not csv_files:
        raise FileNotFoundError(f"No experiment_results_*.csv file found in {directory}")
    
    if len(csv_files) > 1:
        print(f"Warning: Multiple CSV files found in {directory}, using the first one: {csv_files[0]}")
    
    return csv_files[0]

def load_experiment_data(directory):
    """Load experiment data from a directory"""
    csv_file = find_csv_file(directory)
    try:
        df = pd.read_csv(csv_file)
        print(f"Loaded {len(df)} data points from {csv_file}")
        return df
    except Exception as e:
        print(f"Error reading CSV file {csv_file}: {e}")
        sys.exit(1)

def get_column_name(percentile, correction_type):
    """Get the appropriate column name based on percentile and correction type"""
    if correction_type not in ['corrected', 'uncorrected']:
        raise ValueError("correction_type must be 'corrected' or 'uncorrected'")
    
    if percentile == 50:
        return f'{correction_type}_p50'
    elif percentile == 99:
        return f'{correction_type}_p99'
    elif percentile == 99.9:
        return f'{correction_type}_p99_9'
    elif percentile == 'avg':
        return f'{correction_type}_avg'
    else:
        raise ValueError("percentile must be 50, 99, 99.9, or 'avg'")

def plot_comparison(df_http, df_machnet, percentile, correction_type, output_dir, output_prefix="latency_comparison", log_scale=False):
    """Plot comparison of latency curves between HTTP and MachNet"""
    setup_gnuplot_style()
    
    fig, ax = plt.subplots(1, 1, figsize=(12, 8))
    
    # Define colors and markers
    colors = ['#e41a1c', '#377eb8']  # Red for HTTP, Blue for MachNet
    markers = ['o', 's']
    
    # Get column name for the specified percentile and correction type
    column_name = get_column_name(percentile, correction_type)
    
    # Filter successful experiments only
    df_http_success = df_http[df_http['success'] == True].copy()
    df_machnet_success = df_machnet[df_machnet['success'] == True].copy()
    
    if len(df_http_success) == 0:
        print("No successful HTTP experiments found")
        return
    
    if len(df_machnet_success) == 0:
        print("No successful MachNet experiments found")
        return
    
    # Plot HTTP data
    ax.plot(df_http_success['rate'], df_http_success[column_name], 
            color=colors[0], linestyle='-', marker=markers[0], 
            label=f'HTTP ({correction_type.title()})', 
            markerfacecolor='white', markeredgecolor=colors[0])
    
    # Plot MachNet data
    ax.plot(df_machnet_success['rate'], df_machnet_success[column_name], 
            color=colors[1], linestyle='-', marker=markers[1], 
            label=f'MachNet ({correction_type.title()})', 
            markerfacecolor='white', markeredgecolor=colors[1])
    
    # Set labels and title
    ax.set_xlabel('Load (requests/sec)')
    ax.set_ylabel('Latency (μs)')
    
    if percentile == 'avg':
        title_suffix = 'Average'
    elif percentile == 50:
        title_suffix = 'P50 (Median)'
    elif percentile == 99:
        title_suffix = 'P99'
    elif percentile == 99.9:
        title_suffix = 'P99.9'
    
    ax.set_title(f'{title_suffix} Latency Comparison: HTTP vs MachNet ({correction_type.title()})')
    ax.legend()
    ax.grid(True, alpha=0.3)
    
    # Set x-axis limits
    ax.set_xlim(left=0)
    
    # Set y-axis scale and limits
    if log_scale:
        ax.set_yscale('log')
        # For log scale, set a minimum value to avoid log(0) issues
        min_latency = min(df_http_success[column_name].min(), df_machnet_success[column_name].min())
        ax.set_ylim(bottom=max(0.1, min_latency * 0.5))  # Set bottom to avoid log(0)
    else:
        ax.set_ylim(bottom=0)
        # Auto-scale but with some reasonable limits for readability
        max_rate = max(df_http_success['rate'].max(), df_machnet_success['rate'].max())
        max_latency = max(df_http_success[column_name].max(), df_machnet_success[column_name].max())
        
        # If latency values are extremely high (due to saturation), cap the y-axis for better visibility
        if max_latency > 10000:  # 10ms
            print(f"Warning: Maximum latency is {max_latency:.0f} μs. Consider using log scale or filtering data.")
            # Set a reasonable upper limit but allow manual override
            ax.set_ylim([0, min(1000, max_latency)])  # Cap at 1ms unless higher values are reasonable
    
    plt.tight_layout()
    
    # Save the plot
    percentile_str = str(percentile).replace('.', '_')
    scale_suffix = "_log" if log_scale else ""
    output_file = output_dir / f"{output_prefix}_p{percentile_str}_{correction_type}{scale_suffix}.png"
    plt.savefig(output_file, dpi=300, bbox_inches='tight')
    print(f"Saved plot: {output_file}")
    
    plt.show()
    
    # Print summary statistics
    print(f"\nSummary Statistics for {title_suffix} ({correction_type.title()}):")
    print(f"HTTP - Rate range: {df_http_success['rate'].min():.0f} - {df_http_success['rate'].max():.0f} req/sec")
    print(f"HTTP - Latency range: {df_http_success[column_name].min():.1f} - {df_http_success[column_name].max():.1f} μs")
    print(f"MachNet - Rate range: {df_machnet_success['rate'].min():.0f} - {df_machnet_success['rate'].max():.0f} req/sec")
    print(f"MachNet - Latency range: {df_machnet_success[column_name].min():.1f} - {df_machnet_success[column_name].max():.1f} μs")

def plot_multiple_percentiles(df_http, df_machnet, correction_type, output_dir, output_prefix="latency_comparison", log_scale=False):
    """Plot multiple percentiles for comparison"""
    setup_gnuplot_style()
    
    fig, ax = plt.subplots(1, 1, figsize=(14, 8))
    
    # Define colors and styles
    http_color = '#e41a1c'  # Red
    machnet_color = '#377eb8'  # Blue
    percentiles = [('avg', 'Average'), (50, 'P50'), (99, 'P99'), (99.9, 'P99.9')]
    line_styles = ['-', '--', '-.', ':']
    markers = ['o', 's', '^', 'v']
    
    # Filter successful experiments only
    df_http_success = df_http[df_http['success'] == True].copy()
    df_machnet_success = df_machnet[df_machnet['success'] == True].copy()
    
    # Plot HTTP data
    for i, (percentile, label) in enumerate(percentiles):
        column_name = get_column_name(percentile, correction_type)
        ax.plot(df_http_success['rate'], df_http_success[column_name], 
                color=http_color, linestyle=line_styles[i], marker=markers[i], 
                label=f'HTTP {label}', markerfacecolor='white', markeredgecolor=http_color,
                alpha=0.8)
    
    # Plot MachNet data
    for i, (percentile, label) in enumerate(percentiles):
        column_name = get_column_name(percentile, correction_type)
        ax.plot(df_machnet_success['rate'], df_machnet_success[column_name], 
                color=machnet_color, linestyle=line_styles[i], marker=markers[i], 
                label=f'MachNet {label}', markerfacecolor='white', markeredgecolor=machnet_color,
                alpha=0.8)
    
    ax.set_xlabel('Load (requests/sec)')
    ax.set_ylabel('Latency (μs)')
    ax.set_title(f'Latency Comparison: HTTP vs MachNet - All Percentiles ({correction_type.title()})')
    ax.legend(bbox_to_anchor=(1.05, 1), loc='upper left')
    ax.grid(True, alpha=0.3)
    
    # Set x-axis limits
    ax.set_xlim(left=0)
    
    # Set y-axis scale and limits
    if log_scale:
        ax.set_yscale('log')
        # For log scale, find the minimum positive latency value across all data
        all_latencies = []
        for percentile, _ in [('avg', 'Average'), (50, 'P50'), (99, 'P99'), (99.9, 'P99.9')]:
            column_name = get_column_name(percentile, correction_type)
            all_latencies.extend(df_http_success[column_name].tolist())
            all_latencies.extend(df_machnet_success[column_name].tolist())
        min_latency = min([lat for lat in all_latencies if lat > 0])
        ax.set_ylim(bottom=max(0.1, min_latency * 0.5))
    else:
        ax.set_ylim(bottom=0)
    
    plt.tight_layout()
    
    # Save the plot
    scale_suffix = "_log" if log_scale else ""
    output_file = output_dir / f"{output_prefix}_all_{correction_type}{scale_suffix}.png"
    plt.savefig(output_file, dpi=300, bbox_inches='tight')
    print(f"Saved plot: {output_file}")
    
    plt.show()

def main():
    parser = argparse.ArgumentParser(description='Compare latency curves between HTTP and MachNet experiments')
    parser.add_argument('http_dir', help='Directory containing HTTP experiment results')
    parser.add_argument('machnet_dir', help='Directory containing MachNet experiment results')
    parser.add_argument('output_dir', help='Output directory for plots')
    parser.add_argument('-p', '--percentile', 
                        choices=['avg', '50', '99', '99.9'], default='99',
                        help='Percentile to plot (default: 99)')
    parser.add_argument('-c', '--correction', 
                        choices=['corrected', 'uncorrected'], default='corrected',
                        help='Use corrected or uncorrected latency measurements (default: corrected)')
    parser.add_argument('-o', '--output', default='latency_comparison', 
                        help='Output file prefix (default: latency_comparison)')
    parser.add_argument('--all-percentiles', action='store_true',
                        help='Plot all percentiles in a single chart')
    parser.add_argument('--log-scale', action='store_true',
                        help='Use logarithmic scale for y-axis (latency)')
    
    args = parser.parse_args()
    
    # Create output directory
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    
    # Check if directories exist
    http_dir = Path(args.http_dir)
    machnet_dir = Path(args.machnet_dir)
    
    if not http_dir.exists():
        print(f"Error: HTTP directory {args.http_dir} not found")
        sys.exit(1)
    
    if not machnet_dir.exists():
        print(f"Error: MachNet directory {args.machnet_dir} not found")
        sys.exit(1)
    
    # Load data from both directories
    print("Loading HTTP experiment data...")
    df_http = load_experiment_data(http_dir)
    
    print("Loading MachNet experiment data...")
    df_machnet = load_experiment_data(machnet_dir)
    
    # Convert percentile to appropriate type
    if args.percentile == 'avg':
        percentile = 'avg'
    else:
        percentile = float(args.percentile)
    
    # Generate plots
    if args.all_percentiles:
        plot_multiple_percentiles(df_http, df_machnet, args.correction, output_dir, args.output, args.log_scale)
    else:
        plot_comparison(df_http, df_machnet, percentile, args.correction, output_dir, args.output, args.log_scale)

if __name__ == "__main__":
    main() 