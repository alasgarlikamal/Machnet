use std::time::Instant;

use log::info;

#[derive(Debug, Clone, Copy)]
pub struct StatsInstance {
    pub tx_success: u64,
    pub tx_bytes: u64,
    pub rx_count: u64,
    pub rx_bytes: u64,
    pub err_tx_drops: u64,
}

impl StatsInstance {
    pub fn new() -> Self {
        Self {
            tx_success: 0,
            tx_bytes: 0,
            rx_count: 0,
            rx_bytes: 0,
            err_tx_drops: 0,
        }
    }
}

pub struct Stats {
    pub current: StatsInstance,
    pub prev: StatsInstance,
    pub last_measurement_time: Instant,
}

impl Stats {
    pub fn new() -> Self {
        Self {
            current: StatsInstance::new(),
            prev: StatsInstance::new(),
            last_measurement_time: Instant::now(),
        }
    }
}

pub fn report_stats(stats: &mut Stats) {
    let now = Instant::now();
    let sec_elapsed = now.duration_since(stats.last_measurement_time).as_secs();

    let cur = stats.current;
    let prev = stats.prev;

    if sec_elapsed >= 1 {
        let msg_sent = (cur.tx_success - prev.tx_success) as f64;
        let tx_kmps = msg_sent / (1_000.0 * sec_elapsed as f64);
        let tx_gbps = ((cur.tx_bytes - prev.tx_bytes) as f64 * 8.0) / (sec_elapsed as f64 * 1e9);

        let msg_received = (cur.rx_count - prev.rx_count) as f64;
        let rx_kmps = msg_received / (1_000.0 * sec_elapsed as f64);
        let rx_gbps = ((cur.rx_bytes - prev.rx_bytes) as f64 * 8.0) / (sec_elapsed as f64 * 1e9);

        let msg_dropped = cur.err_tx_drops - prev.err_tx_drops;

        let mut drop_stats_str = String::new();
        if msg_dropped > 0 {
            drop_stats_str.push_str(format!(", TX drops: {}", msg_dropped).as_str())
        }

        info!(
            "TX/RX (msg/sec, Gbps): ({:.1}K/{:.1}K, {:.3}/{:.3}). {}",
            tx_kmps, rx_kmps, tx_gbps, rx_gbps, drop_stats_str
        );

        stats.last_measurement_time = now;
        stats.prev = stats.current;
    }
}
