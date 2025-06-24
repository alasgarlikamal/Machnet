#!/usr/bin/env python3

import json
import subprocess
import os
import re
import csv
from datetime import datetime
from pathlib import Path
import shutil

def parse_latency_output(output_text):
    """Parse webclient output to extract latency metrics"""
    metrics = {
        'actual_req_sec': 0.0,
        'corrected_avg': 0.0,
        'corrected_p50': 0.0,
        'corrected_p99': 0.0,
        'corrected_p99_9': 0.0,
        'uncorrected_avg': 0.0,
        'uncorrected_p50': 0.0,
        'uncorrected_p99': 0.0,
        'uncorrected_p99_9': 0.0
    }
    
    # Extract actual requests/sec
    req_sec_match = re.search(r'Requests/sec:\s+([\d.]+)', output_text)
    if req_sec_match:
        metrics['actual_req_sec'] = float(req_sec_match.group(1))
    
    # Find the two latency sections including their detailed spectrum parts
    corrected_section = re.search(
        r'Latency Distribution \(HdrHistogram - Recorded Latency\)\s*\n(.*?)(?=\n\s*Latency Distribution \(HdrHistogram - Uncorrected|----------------------------------------------------------)', 
        output_text, re.DOTALL
    )
    
    uncorrected_section = re.search(
        r'Latency Distribution \(HdrHistogram - Uncorrected Latency.*?\)\s*\n(.*?)(?=\n\s*----------------------------------------------------------)', 
        output_text, re.DOTALL
    )
    
    def parse_percentiles_and_mean(section_text):
        """Parse percentiles and mean from a latency section"""
        data = {'avg': 0.0, 'p50': 0.0, 'p99': 0.0, 'p99_9': 0.0}
        
        if not section_text:
            return data
            
        # Extract percentiles - handle various formatting issues
        p50_match = re.search(r'50\.000%\s*([\d.]+)(?:us?)?', section_text)
        p99_match = re.search(r'99\.000%\s*([\d.]+)(?:us?)?', section_text)
        p99_9_match = re.search(r'99\.900%\s*([\d.]+)(?:us?)?', section_text)
        
        if p50_match:
            data['p50'] = float(p50_match.group(1))
        if p99_match:
            data['p99'] = float(p99_match.group(1))
        if p99_9_match:
            data['p99_9'] = float(p99_9_match.group(1))
            
        # Extract mean from detailed spectrum section - already in microseconds
        mean_match = re.search(r'#\[Mean\s*=\s*([\d.]+)', section_text)
        if mean_match:
            data['avg'] = float(mean_match.group(1))  # Already in microseconds
        else:
            # Try alternative patterns
            alt_mean_match = re.search(r'Mean\s*=\s*([\d.]+)', section_text)
            if alt_mean_match:
                data['avg'] = float(alt_mean_match.group(1))
            else:
                print("[DEBUG] No mean found in section. Section text (first 500 chars):")
                print(section_text[:500])
            
        return data
    
    # Parse corrected latencies
    if corrected_section:
        corrected_data = parse_percentiles_and_mean(corrected_section.group(1))
        metrics['corrected_avg'] = corrected_data['avg']
        metrics['corrected_p50'] = corrected_data['p50']
        metrics['corrected_p99'] = corrected_data['p99']
        metrics['corrected_p99_9'] = corrected_data['p99_9']
    else:
        print("[DEBUG] No corrected latency section found")
    
    # Parse uncorrected latencies  
    if uncorrected_section:
        uncorrected_data = parse_percentiles_and_mean(uncorrected_section.group(1))
        metrics['uncorrected_avg'] = uncorrected_data['avg']
        metrics['uncorrected_p50'] = uncorrected_data['p50']
        metrics['uncorrected_p99'] = uncorrected_data['p99']
        metrics['uncorrected_p99_9'] = uncorrected_data['p99_9']
    else:
        print("[DEBUG] No uncorrected latency section found")
    
    return metrics

def parse_ranges(range_strings):
    """Parse range strings in format 'start:stop:step' and return sorted unique rates"""
    rates = set()
    
    for range_str in range_strings:
        try:
            parts = range_str.split(':')
            if len(parts) != 3:
                print(f"Warning: Invalid range format '{range_str}'. Expected 'start:stop:step'")
                continue
                
            start, stop, step = map(int, parts)
            
            # Generate range using Python's range logic: range(start, stop+1, step)
            # This includes 'stop' value if it's reachable by the step
            current = start
            while current <= stop:
                rates.add(current)
                current += step
                
        except ValueError as e:
            print(f"Warning: Error parsing range '{range_str}': {e}")
            continue
    
    return sorted(rates)

def run_experiment(config, rate, webclient_path, url, output_dir):
    """Run a single experiment with given rate"""
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    output_file = output_dir / f"{timestamp}_{rate}.txt"
    
    # Build command
    cmd = [
        str(webclient_path),
        url,
        "-c", str(config['connections']),
        "-t", str(config['threads']),
        "-R", str(rate),
        "-d", str(config['duration']),
        "-L", "-U"  # Enable both latency outputs
    ]
    
    print(f"Running experiment: rate={rate}, cmd={' '.join(cmd)}")
    
    try:
        # Run the command
        result = subprocess.run(
            cmd, 
            capture_output=True, 
            text=True, 
            timeout=config['duration'] + 30  # Add buffer time
        )
        
        # Prepare output content
        output_content = f"Command: {' '.join(cmd)}\n"
        output_content += f"Return code: {result.returncode}\n"
        output_content += f"STDOUT:\n{result.stdout}\n"
        if result.stderr:
            output_content += f"STDERR:\n{result.stderr}\n"
        
        # Save raw output
        with open(output_file, 'w') as f:
            f.write(output_content)
        
        # Parse metrics if successful
        if result.returncode == 0:
            metrics = parse_latency_output(result.stdout)
            metrics['rate'] = rate
            metrics['success'] = True
            print("Metrics: ", metrics)
            return metrics
        else:
            print(f"Command failed with return code {result.returncode}")
            return {'rate': rate, 'success': False}
            
    except subprocess.TimeoutExpired:
        print(f"Experiment timed out for rate {rate}")
        return {'rate': rate, 'success': False}
    except Exception as e:
        print(f"Error running experiment for rate {rate}: {e}")
        return {'rate': rate, 'success': False}

def main():
    import argparse
    
    parser = argparse.ArgumentParser(description='Run load testing experiments with webclient')
    parser.add_argument('output_dir', help='Output directory for results and logs')
    parser.add_argument('-c', '--config', default='experiment.json', 
                        help='Configuration file (default: experiment.json)')
    
    args = parser.parse_args()
    
    script_dir = Path(__file__).parent
    config_file = script_dir / args.config
    output_dir = Path(args.output_dir)
    
    # Create output directory
    output_dir.mkdir(parents=True, exist_ok=True)
    
    # Ensure wrk.lua is available
    # ensure_wrk_lua_available(script_dir)
    
    # Read configuration
    try:
        with open(config_file, 'r') as f:
            config = json.load(f)
    except FileNotFoundError:
        print(f"Error: {config_file} not found")
        return
    except json.JSONDecodeError as e:
        print(f"Error parsing {config_file}: {e}")
        return
    
    # Validate required fields
    required_fields = ['connections', 'threads', 'range', 'duration']
    missing_fields = [field for field in required_fields if field not in config]
    if missing_fields:
        print(f"Error: Missing required fields in config: {missing_fields}")
        return
    
    # Find webclient binary
    webclient_paths = [
        script_dir / "../../release_build/examples/webclient/webclient",
        script_dir / "webclient/webclient",
        script_dir / "../../build/examples/webclient/webclient"
    ]
    
    webclient_path = None
    for path in webclient_paths:
        if path.exists():
            webclient_path = path
            break
    
    if not webclient_path:
        print("Error: Could not find webclient binary")
        print("Looked for:")
        for path in webclient_paths:
            print(f"  {path}")
        return
    
    # Default URL (can be overridden in config)
    url = config.get('url', 'http://10.10.1.1:8000/index.html')
    
    print(f"Using webclient: {webclient_path}")
    print(f"Target URL: {url}")
    print(f"Configuration: {config}")
    
    # Parse rate ranges
    rates = parse_ranges(config['range'])
    if not rates:
        print("Error: No valid rates found in range specification")
        return
    
    print(f"Testing rates: {rates}")
    
    # Run experiments
    results = []
    for rate in rates:
        result = run_experiment(config, rate, webclient_path, url, output_dir)
        results.append(result)
    
    # Generate CSV output
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    csv_file = output_dir / f"experiment_results_{timestamp}.csv"
    
    print(f"\nWriting results to {csv_file}")
    
    with open(csv_file, 'w', newline='') as f:
        fieldnames = [
            'rate', 'actual_req_sec',
            'corrected_avg', 'corrected_p50', 'corrected_p99', 'corrected_p99_9',
            'uncorrected_avg', 'uncorrected_p50', 'uncorrected_p99', 'uncorrected_p99_9',
            'success'
        ]
        
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        
        for result in results:
            if result.get('success', False):
                writer.writerow({
                    'rate': result['rate'],
                    'actual_req_sec': result['actual_req_sec'],
                    'corrected_avg': result['corrected_avg'],
                    'corrected_p50': result['corrected_p50'],
                    'corrected_p99': result['corrected_p99'],
                    'corrected_p99_9': result['corrected_p99_9'],
                    'uncorrected_avg': result['uncorrected_avg'],
                    'uncorrected_p50': result['uncorrected_p50'],
                    'uncorrected_p99': result['uncorrected_p99'],
                    'uncorrected_p99_9': result['uncorrected_p99_9'],
                    'success': True
                })
            else:
                writer.writerow({
                    'rate': result['rate'],
                    'success': False
                })
    
    # Print summary
    successful_runs = [r for r in results if r.get('success', False)]
    print(f"\nExperiment Summary:")
    print(f"Total runs: {len(results)}")
    print(f"Successful runs: {len(successful_runs)}")
    print(f"Failed runs: {len(results) - len(successful_runs)}")
    print(f"Results saved to: {csv_file}")
    print(f"Raw outputs in: {output_dir}")

# def ensure_wrk_lua_available(script_dir):
#     """Ensure wrk.lua is available in the current directory"""
#     wrk_lua_source = script_dir / "webclient/src/wrk.lua"
#     wrk_lua_dest = script_dir / "wrk.lua"
    
#     if wrk_lua_source.exists() and not wrk_lua_dest.exists():
#         print(f"Copying {wrk_lua_source} to {wrk_lua_dest}")
#         shutil.copy2(wrk_lua_source, wrk_lua_dest)
#     elif not wrk_lua_source.exists():
#         print(f"Warning: {wrk_lua_source} not found")
    
#     return wrk_lua_dest.exists()

if __name__ == "__main__":
    main() 