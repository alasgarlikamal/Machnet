## Scripts

### `run_experiments.py`
Runs load testing experiments using the webclient with configurable rate ranges.

**Usage:**
```bash
python3 run_experiments.py <output_dir> [-c config_file]
```

**Arguments:**
- `output_dir`: Required. Directory where results and logs will be saved
- `-c, --config`: Optional. Configuration file (default: experiment.json)

**Examples:**
```bash
# Run experiments with default config, save to ./results
python3 run_experiments.py ./results

# Run with custom config, save to /tmp/my_tests
python3 run_experiments.py /tmp/my_tests -c my_experiment.json

# Run with absolute path
python3 run_experiments.py /home/user/load_tests
```

### `plot_latency.py`
Generates load-latency plots from experiment CSV results.

**Usage:**
```bash
python3 plot_latency.py <csv_file> <output_dir> [options]
```

**Arguments:**
- `csv_file`: Required. CSV file with experiment results
- `output_dir`: Required. Directory where plot images will be saved
- `-o, --output`: Output file prefix (default: latency_plot)
- `-p, --percentile`: Plot specific percentile only (50, 99, or 99.9)
- `--all`: Plot all percentiles in single figure (default)
- `--combined`: Plot side-by-side corrected vs uncorrected (deprecated)
- `--throughput`: Plot throughput comparison

**Examples:**
```bash
# Default: All latencies in single plot
python3 plot_latency.py results/experiment_results_20250623_123456.csv ./plots

# Plot only P99 latency
python3 plot_latency.py results/experiment_results_20250623_123456.csv ./plots -p 99

# Plot throughput comparison
python3 plot_latency.py results/experiment_results_20250623_123456.csv ./plots --throughput

# Custom output prefix
python3 plot_latency.py results/experiment_results_20250623_123456.csv ./plots -o my_analysis
```

## Configuration Format

### `experiment.json`
```json
{
    "connections": 1,
    "threads": 1,
    "range": [
        "1000:5000:1000",
        "10000:50000:10000"
    ],
    "duration": 10,
    "url": "http://10.10.1.1:8000/index.html"
}
```

**Range Format:** `"start:stop:step"`
- Multiple ranges are supported and will be combined
- Example: `["10:50:10", "100:500:100"]` generates rates: 10,20,30,40,50,100,200,300,400,500

## Output Files

### Experiment Results
- **CSV files**: `experiment_results_YYYYMMDD_HHMMSS.csv` with latency metrics
- **Raw logs**: Individual `.txt` files for each rate tested

### Plot Files
- **`*_all.png`**: All percentiles in single figure
- **`*_p50.png`**: P50 latency only
- **`*_p99.png`**: P99 latency only
- **`*_p99_9.png`**: P99.9 latency only
- **`*_throughput.png`**: Throughput comparison
- **`*_combined.png`**: Side-by-side layout
