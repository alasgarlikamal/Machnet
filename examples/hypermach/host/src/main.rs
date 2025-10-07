mod args;
mod stats;

use std::{
    env,
    sync::{
        atomic::{AtomicBool, AtomicUsize, Ordering},
        Arc, Mutex,
    },
    thread,
    time::{Duration, Instant},
};

use crate::args::Args;

use anyhow::Result;
use hyperlight_host::{
    func::ReturnType,
    sandbox_state::{sandbox::EvolvableSandbox, transition::Noop},
    MultiUseGuestCallContext, MultiUseSandbox, UninitializedSandbox,
};
use log::{debug, info};
use machnet::{
    machnet_attach, machnet_init, machnet_listen, machnet_recv, machnet_send, MachnetChannel,
    MachnetFlow,
};
use rand::Rng;
use signal_hook::{consts::SIGINT, iterator::Signals};
use stats::{report_stats, Stats};
use threadpool::ThreadPool;

struct AppHdr {
    window_slot: u64,
}

#[derive(Debug, Clone, Copy, PartialEq)]
enum CommunicationStyle {
    RoundRobin,
    Random,
    Broadcast,
}

impl CommunicationStyle {
    fn from_str(s: &str) -> Result<Self> {
        match s.to_lowercase().as_str() {
            "round-robin" => Ok(CommunicationStyle::RoundRobin),
            "random" => Ok(CommunicationStyle::Random),
            "broadcast" => Ok(CommunicationStyle::Broadcast),
            _ => Err(anyhow::anyhow!(
                "Invalid communication style: {}. Valid options: round-robin, random, broadcast",
                s
            )),
        }
    }
}

// Flag to keep the server running
static G_KEEP_RUNNING: AtomicBool = AtomicBool::new(true);

// Machnet constants
const MACHNET_MSG_MAX_LEN: usize = 64; // 8 * 1 << 20; // 8MB

// Hyperlight guest binary path
const GUEST_PATH: &str = "/users/vj2267/machnet/examples/hypermach/guest/target/x86_64-unknown-none/release/hypermach-guest";

fn setup_signal_handler() {
    let mut signals = Signals::new(&[SIGINT]).unwrap();

    thread::spawn(move || {
        for _sig in signals.forever() {
            log::warn!("Signal received!");
            G_KEEP_RUNNING.store(false, Ordering::SeqCst);
        }
    });
}

// Creates and returns a sandbox with guest binary
fn init_plain_sandbox() -> MultiUseSandbox {
    let sandbox = UninitializedSandbox::new(
        hyperlight_host::GuestBinary::FilePath(GUEST_PATH.to_string()),
        None,
        None,
        None,
    )
    .unwrap();

    let multi_use_sandbox = sandbox.evolve(Noop::default()).unwrap();
    multi_use_sandbox
}

fn server(
    guest_contexts: Vec<Arc<Mutex<MultiUseGuestCallContext>>>,
    mut channel: MachnetChannel,
    msg_size: u64,
    msg_window: u64,
    comm_style: CommunicationStyle,
) {
    let _ = msg_window;
    info!(
        "Server: Starting with {} guests, style: {:?}",
        guest_contexts.len(),
        comm_style
    );

    let mut stats = Stats::new();
    let round_robin_index = AtomicUsize::new(0);
    let mut rng = rand::thread_rng();

    // Create thread pool for broadcast mode - limit to available cores
    let num_workers = (num_cpus::get() - 1).min(guest_contexts.len());
    let pool = ThreadPool::new(num_workers);
    info!("Server: Thread pool created with {} workers", num_workers);

    loop {
        if !G_KEEP_RUNNING.load(Ordering::SeqCst) {
            info!("Server: Stopping...");
            break;
        }

        let mut stats_current = stats.current;

        let mut rx_flow = MachnetFlow::default();
        let mut rx_message: Vec<u8> = vec![0; MACHNET_MSG_MAX_LEN];
        let tx_message: Vec<u8> = vec![0; MACHNET_MSG_MAX_LEN];
        let rx_message_size = rx_message.len() as u64;

        let rx_size = machnet_recv(&mut channel, &mut rx_message, rx_message_size, &mut rx_flow);

        if rx_size <= 0 {
            continue;
        }

        stats_current.rx_count += 1;
        stats_current.rx_bytes += rx_size as u64;

        let req_hdr = unsafe { &*(rx_message.as_ptr() as *const AppHdr) };
        let window_slot = req_hdr.window_slot as usize;
        debug!("Server: Received message with window slot {}", window_slot);

        // Perform guest call(s) based on communication style
        let guest_call_start = Instant::now();

        match comm_style {
            CommunicationStyle::RoundRobin => {
                let idx = round_robin_index.fetch_add(1, Ordering::Relaxed) % guest_contexts.len();
                let mut guest_ctx = guest_contexts[idx].lock().unwrap();

                let guest_result = guest_ctx
                    .call(
                        "DirectEcho",
                        ReturnType::VecBytes,
                        Some(vec![hyperlight_host::func::ParameterValue::VecBytes(
                            rx_message.clone(),
                        )]),
                    )
                    .unwrap();

                match guest_result {
                    hyperlight_host::func::ReturnValue::VecBytes(result) => {
                        assert_eq!(result, rx_message, "Echo output does not match input");
                    }
                    _ => panic!("Unexpected return type from DirectEcho function"),
                }
            }

            CommunicationStyle::Random => {
                let idx = rng.gen_range(0..guest_contexts.len());
                let mut guest_ctx = guest_contexts[idx].lock().unwrap();

                let guest_result = guest_ctx
                    .call(
                        "DirectEcho",
                        ReturnType::VecBytes,
                        Some(vec![hyperlight_host::func::ParameterValue::VecBytes(
                            rx_message.clone(),
                        )]),
                    )
                    .unwrap();

                match guest_result {
                    hyperlight_host::func::ReturnValue::VecBytes(result) => {
                        assert_eq!(result, rx_message, "Echo output does not match input");
                    }
                    _ => panic!("Unexpected return type from DirectEcho function"),
                }
            }

            CommunicationStyle::Broadcast => {
                // Broadcast to all guests in parallel using thread pool
                let (tx, rx) = std::sync::mpsc::channel();

                for (i, guest_ctx_arc) in guest_contexts.iter().enumerate() {
                    let guest_ctx_clone = Arc::clone(guest_ctx_arc);
                    let rx_message_clone = rx_message.clone();
                    let tx_clone = tx.clone();

                    pool.execute(move || {
                        let mut guest_ctx = guest_ctx_clone.lock().unwrap();
                        let guest_result = guest_ctx
                            .call(
                                "DirectEcho",
                                ReturnType::VecBytes,
                                Some(vec![hyperlight_host::func::ParameterValue::VecBytes(
                                    rx_message_clone.clone(),
                                )]),
                            )
                            .unwrap();

                        match guest_result {
                            hyperlight_host::func::ReturnValue::VecBytes(result) => {
                                tx_clone.send((i, result)).unwrap();
                            }
                            _ => panic!("Unexpected return type from DirectEcho function"),
                        }
                    });
                }

                drop(tx); // Drop the original sender

                // Collect all results
                let mut all_results = Vec::new();
                for (_, result) in rx.iter() {
                    all_results.push(result);
                }

                // Verify all results match the input
                for result in all_results.iter() {
                    assert_eq!(*result, rx_message, "Echo output does not match input");
                }
            }
        }

        let guest_call_duration = guest_call_start.elapsed().as_micros() as u64;

        // Update guest call statistics
        stats_current.guest_call_count += 1;
        stats_current.guest_call_total_us += guest_call_duration;
        stats_current.guest_call_min_us = stats_current.guest_call_min_us.min(guest_call_duration);
        stats_current.guest_call_max_us = stats_current.guest_call_max_us.max(guest_call_duration);

        // Send the response back to the client
        let resp_hdr = unsafe { &mut *(tx_message.as_ptr() as *mut AppHdr) };
        resp_hdr.window_slot = req_hdr.window_slot;

        // src_ip, src_port, dst_ip, dst_port
        let tx_flow = MachnetFlow::new(
            rx_flow.dst_ip,
            rx_flow.dst_port,
            rx_flow.src_ip,
            rx_flow.src_port,
        );

        let ret = machnet_send(&mut channel, tx_flow, &tx_message, msg_size);

        match ret {
            0 => {
                stats_current.tx_success += 1;
                stats_current.tx_bytes += msg_size;
            }
            _ => stats_current.err_tx_drops += 1,
        }

        // Commit stats changes
        stats.current = stats_current;

        report_stats(&mut stats);
    }
}

fn main() -> Result<()> {
    unsafe { env::set_var("RUST_LOG", "hypermach_host=info") };
    env_logger::init();

    let args: Args = Args::parse(std::env::args().collect())?;
    let comm_style = CommunicationStyle::from_str(args.communication_style())?;

    setup_signal_handler();

    assert!(
        args.msg_size() > size_of::<AppHdr>().try_into().unwrap(),
        "Message size is too small."
    );

    assert_eq!(machnet_init(), 0, "Failed to initialize Machnet library.");

    let mut channel = machnet_attach().unwrap();

    let ret = machnet_listen(&mut channel, args.server_ip(), args.port());
    assert_eq!(ret, 0, "Failed to listen on port {}", args.port());

    info!("[LISTENING] [{}:{}]", args.server_ip(), args.port());

    // Initialize multiple guest contexts
    info!("Initializing {} guest(s)...", args.num_guests());
    let mut guest_contexts = Vec::new();
    for i in 0..args.num_guests() {
        info!("Creating guest sandbox {}/{}", i + 1, args.num_guests());
        let sandbox = init_plain_sandbox();
        let guest_ctx = sandbox.new_call_context();
        guest_contexts.push(Arc::new(Mutex::new(guest_ctx)));
    }
    info!(
        "All {} guest(s) initialized successfully",
        args.num_guests()
    );

    let msg_size = args.msg_size();
    let msg_window = args.msg_window();

    let datapath_thread =
        thread::spawn(move || server(guest_contexts, channel, msg_size, msg_window, comm_style));

    while G_KEEP_RUNNING.load(Ordering::SeqCst) {
        thread::sleep(Duration::from_secs(5));
    }

    datapath_thread.join().unwrap();
    Ok(())
}
