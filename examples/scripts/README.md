## Machnet Running Web Server/Client

For now, webserver needs a `web_root` directory and `index.html` file there to serve - TODO: find a workaround for this.


## Running HTTP over TCP
### Client
- Get wrk2 with `git clone https://github.com/giltene/wrk2.git` and apply [patch](../webclient/usec.patch) as `git apply usec.patch`
	- Patch changes all output to microseconds to be consistent with plotting and running experiments
- Build with `make`, later use the binary path in experiment configuration file.
### Server
- Get mongoose with `git clone https://github.com/cesanta/mongoose.git` and use [http_server](https://github.com/cesanta/mongoose/tree/master/tutorials/http/http-server) 
- Build with `make` and run. Make sure that you have empty index.html file in the same folder to serve.

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
Generates load-latency plots from experiment CSV results. Supports both single CSV files and JSON configuration for comparing multiple datasets.

**Usage:**
```bash
python3 plot_latency.py <input_file> <output_dir> [options]
```

**Arguments:**
- `input_file`: Required. CSV file with experiment results OR JSON configuration file
- `output_dir`: Required. Directory where plot images will be saved
- `-o, --output`: Output file prefix (default: latency_plot)
- `-p, --percentile`: Plot specific percentile only (50, 99, or 99.9)
- `--all`: Plot all percentiles in single figure (default)
- `--combined`: Plot side-by-side corrected vs uncorrected (deprecated)
- `--throughput`: Plot throughput comparison
- `--json`: Force treat input as JSON configuration file

**Single CSV Examples:**
```bash
# Default: All latencies in single plot
python3 plot_latency.py results/experiment_results_20250623_123456.csv ./plots

# Plot only P99 latency
python3 plot_latency.py results/experiment_results_20250623_123456.csv ./plots -p 99

# Plot throughput comparison
python3 plot_latency.py results/experiment_results_20250623_123456.csv ./plots --throughput
```

**Multi-Dataset Comparison Examples:**
```bash
# Compare multiple datasets using JSON config
python3 plot_latency.py plot.json ./plots

# Compare P99 latencies from multiple datasets
python3 plot_latency.py plot.json ./plots -p 99

# Custom output prefix for comparison
python3 plot_latency.py plot.json ./plots -o machnet_vs_http
```

## Configuration Format

### `experiment.json` (for run_experiments.py)
```json
{
    "binary_path": "....",
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

### `plot.json` (for plot_latency.py comparison plots)
```json
{
    "HTTP": "output_http/experiment_results_20250624_161645.csv",
    "Machnet": "output_machnet/experiment_results_20250624_011645.csv"
}
```

**Alternative format:**
```json
[
    "HTTP:output_http/experiment_results_20250624_161645.csv",
    "Machnet:output_machnet/experiment_results_20250624_011645.csv"
]
```

**JSON Configuration Rules:**
- Dictionary format: `{"caption": "csv_file.csv"}`
- List format: `["caption:csv_file.csv"]`
- File paths are relative to the JSON file location
- Absolute paths are also supported
- Only successful experiments from each CSV are plotted

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
