mod args;
mod stats;

use std::{
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, AtomicUsize, Ordering},
    },
    time::{Duration, Instant},
};

use crate::args::{Args, CommStyle};

use ::anyhow::Result;
use futures::future::join_all;
use hdrhistogram::Histogram;
use hyperlight_host::{
    MultiUseGuestCallContext, MultiUseSandbox, UninitializedSandbox,
    func::ReturnType,
    sandbox_state::{sandbox::EvolvableSandbox, transition::Noop},
};
use log::{debug, info};
use machnet::{
    MachnetChannel, MachnetFlow, machnet_attach, machnet_init, machnet_listen, machnet_recv,
    machnet_send,
};
use stats::{Stats, report_stats};

struct AppHdr {
    window_slot: u64,
}

// Machnet constants
const MACHNET_MSG_MAX_LEN: usize = 64; // 8 * 1 << 20; // 8MB

// Hyperlight guest binary path
const GUEST_PATH: &str = "/users/vj2267/machnet/examples/hypermach/guest/target/x86_64-unknown-none/release/hypermach-guest";

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

async fn server<'a>(
    guest_contexts: Vec<Arc<Mutex<MultiUseGuestCallContext>>>,
    mut channel: MachnetChannel<'a>,
    msg_size: u64,
    msg_window: u64,
    comm_style: CommStyle,
    shutdown_flag: Arc<AtomicBool>,
) {
    let _ = msg_window;
    info!(
        "Server: Starting with {} guests and {:?} communication pattern...",
        guest_contexts.len(),
        comm_style
    );

    let mut stats = Stats::new();
    let round_robin_counter = Arc::new(AtomicUsize::new(0));
    let num_guests = guest_contexts.len();

    // HDR Histogram for guest invocation times (tracking nanoseconds, max 1 hour)
    let guest_invocation_histogram = Arc::new(Mutex::new(Histogram::<u64>::new_with_bounds(1, 3_600_000_000_000, 3).unwrap()));

    // Spawn async task to print histogram statistics every 10 seconds
    let histogram_clone = Arc::clone(&guest_invocation_histogram);
    let shutdown_clone = Arc::clone(&shutdown_flag);
    tokio::spawn(async move {
        let mut interval = tokio::time::interval(Duration::from_secs(10));
        loop {
            interval.tick().await;

            if shutdown_clone.load(Ordering::SeqCst) {
                break;
            }

            let mut hist = histogram_clone.lock().unwrap();
            if hist.len() > 0 {
                info!("=== Guest invocation statistics (periodic) ===");
                info!("  Total invocations: {}", hist.len());
                info!("  Min time: {:?}", Duration::from_nanos(hist.min()));
                info!("  Max time: {:?}", Duration::from_nanos(hist.max()));
                info!("  Avg time: {:?}", Duration::from_nanos(hist.mean() as u64));
                info!("  50th percentile: {:?}", Duration::from_nanos(hist.value_at_quantile(0.50)));
                info!("  99th percentile: {:?}", Duration::from_nanos(hist.value_at_quantile(0.99)));
                info!("  99.9th percentile: {:?}", Duration::from_nanos(hist.value_at_quantile(0.999)));
                hist.clear();
            }
        }
    });

    loop {
        // Check for shutdown before processing next message
        if shutdown_flag.load(Ordering::SeqCst) {
            info!("Server: Stopping...");

            // Report final guest invocation statistics
            let mut hist = guest_invocation_histogram.lock().unwrap();
            if hist.len() > 0 {
                info!("=== Final guest invocation statistics ===");
                info!("  Total invocations: {}", hist.len());
                info!("  Min time: {:?}", Duration::from_nanos(hist.min()));
                info!("  Max time: {:?}", Duration::from_nanos(hist.max()));
                info!("  Avg time: {:?}", Duration::from_nanos(hist.mean() as u64));
                info!("  50th percentile: {:?}", Duration::from_nanos(hist.value_at_quantile(0.50)));
                info!("  99th percentile: {:?}", Duration::from_nanos(hist.value_at_quantile(0.99)));
                info!("  99.9th percentile: {:?}", Duration::from_nanos(hist.value_at_quantile(0.999)));
                hist.clear();
            }

            break;
        }

        // Block here until we receive a message - no other async operations can run
        // This ensures we don't receive another message while processing current one
        let mut stats_current = stats.current;

        let mut rx_flow = MachnetFlow::default();
        let mut rx_message: Vec<u8> = vec![0; MACHNET_MSG_MAX_LEN];
        let rx_message_size = rx_message.len() as u64;

        // Blocking receive - will not proceed until a message arrives
        let rx_size = tokio::task::block_in_place(|| {
            machnet_recv(&mut channel, &mut rx_message, rx_message_size, &mut rx_flow)
        });

        if rx_size <= 0 {
            // No message received, yield and check shutdown again
            tokio::task::yield_now().await;
            continue;
        }

        stats_current.rx_count += 1;
        stats_current.rx_bytes += rx_size as u64;

        let req_hdr = unsafe { &*(rx_message.as_ptr() as *const AppHdr) };
        let window_slot = req_hdr.window_slot as usize;
        debug!("Server: Received message with window slot {}", window_slot);

        // === BEGIN MESSAGE PROCESSING ===
        // From this point until we send the response, no new messages will be received
        // All guest invocations complete before we loop back to machnet_recv

        // Process based on communication style
        let start_time = Instant::now();
        let tx_message = match comm_style {
            CommStyle::Broadcast => {
                // Send to all guests in parallel as tasks on the fixed thread pool
                // No new threads are spawned - tasks are scheduled on existing worker threads
                let rx_msg_clone = rx_message.clone();
                let mut tasks = vec![];

                for ctx in &guest_contexts {
                    let ctx_clone = Arc::clone(ctx);
                    let msg_clone = rx_msg_clone.clone();

                    // Use tokio::spawn (not spawn_blocking) to schedule on existing thread pool
                    // Guest call is synchronous, so it will block the thread it runs on,
                    // but no NEW thread is created - it uses one from the fixed pool
                    let task = tokio::task::spawn(async move {
                        // Run the blocking guest call in a task
                        // This blocks one of the 63 worker threads but doesn't spawn a new thread
                        let mut guest_ctx = ctx_clone.lock().unwrap();
                        let guest_result = guest_ctx
                            .call(
                                "DirectEcho",
                                ReturnType::VecBytes,
                                Some(vec![hyperlight_host::func::ParameterValue::VecBytes(
                                    msg_clone,
                                )]),
                            )
                            .unwrap();
                        match guest_result {
                            hyperlight_host::func::ReturnValue::VecBytes(response) => response,
                            _ => panic!("Unexpected return type from DirectEcho function"),
                        }
                    });

                    tasks.push(task);
                }

                // Wait for ALL tasks to complete concurrently
                // join_all awaits all futures at once, enabling true parallelism
                let responses: Vec<Vec<u8>> = join_all(tasks)
                    .await
                    .into_iter()
                    .map(|r| r.unwrap())
                    .collect();

                // Verify all responses match the input
                for response in &responses {
                    assert_eq!(response, &rx_message, "Echo output does not match input");
                }

                // Return the first response (they're all the same)
                responses[0].clone()
            }
            CommStyle::RoundRobin => {
                // Select guest in round-robin fashion
                let guest_idx = round_robin_counter.fetch_add(1, Ordering::SeqCst) % num_guests;
                let ctx_clone = Arc::clone(&guest_contexts[guest_idx]);
                let msg_clone = rx_message.clone();

                let response = tokio::task::spawn(async move {
                    let mut guest_ctx = ctx_clone.lock().unwrap();
                    let guest_result = guest_ctx
                        .call(
                            "DirectEcho",
                            ReturnType::VecBytes,
                            Some(vec![hyperlight_host::func::ParameterValue::VecBytes(
                                msg_clone,
                            )]),
                        )
                        .unwrap();
                    match guest_result {
                        hyperlight_host::func::ReturnValue::VecBytes(response) => response,
                        _ => panic!("Unexpected return type from DirectEcho function"),
                    }
                })
                .await
                .unwrap();

                assert_eq!(response, rx_message, "Echo output does not match input");
                response
            }
            CommStyle::Random => {
                // Select guest randomly
                use rand::Rng;
                let guest_idx = rand::thread_rng().gen_range(0..num_guests);
                let ctx_clone = Arc::clone(&guest_contexts[guest_idx]);
                let msg_clone = rx_message.clone();

                let response = tokio::task::spawn(async move {
                    let mut guest_ctx = ctx_clone.lock().unwrap();
                    let guest_result = guest_ctx
                        .call(
                            "DirectEcho",
                            ReturnType::VecBytes,
                            Some(vec![hyperlight_host::func::ParameterValue::VecBytes(
                                msg_clone,
                            )]),
                        )
                        .unwrap();
                    match guest_result {
                        hyperlight_host::func::ReturnValue::VecBytes(response) => response,
                        _ => panic!("Unexpected return type from DirectEcho function"),
                    }
                })
                .await
                .unwrap();

                assert_eq!(response, rx_message, "Echo output does not match input");
                response
            }
        };
        let invocation_time = start_time.elapsed();
        guest_invocation_histogram.lock().unwrap().record(invocation_time.as_nanos() as u64).ok();

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

        report_stats(&mut stats);

        // === END MESSAGE PROCESSING ===
        // Message fully processed and response sent
        // Now loop back to receive the next message
    }
}

fn main() -> Result<()> {
    unsafe { std::env::set_var("RUST_LOG", "hypermach_host=info") };
    env_logger::init();

    let args: Args = Args::parse(std::env::args().collect())?;

    // Log the number of available CPU cores
    let num_cores = num_cpus::get();
    info!("Available CPU cores: {}", num_cores);

    // Get core IDs
    let core_ids = core_affinity::get_core_ids().unwrap();

    // Fixed thread pool: SKIP CPU 0 (reserved), use CPUs 1 through N
    let worker_core_ids: Vec<_> = core_ids.iter().skip(1).cloned().collect();
    let num_worker_threads = worker_core_ids.len(); // All CPUs except CPU 0

    info!("Configuring Tokio runtime:");
    info!("  Total CPUs on system: {}", num_cores);
    info!("  CPU 0: RESERVED (not used)");
    info!("  CPUs for thread pool: {} (CPUs 1-{})", num_worker_threads, num_cores - 1);
    info!("  Fixed thread pool size: {}", num_worker_threads);
    info!("  Number of guests: {}", args.num_guests());
    info!("  Tasks will be scheduled on the fixed thread pool (no new thread spawning)");

    // Verify we're not using CPU 0
    assert!(num_worker_threads == num_cores - 1,
        "Expected {} threads (all CPUs except CPU 0), got {}", num_cores - 1, num_worker_threads);

    // Build custom runtime with FIXED thread pool
    // Worker threads = all available CPUs
    // All guest calls are scheduled as tasks on this fixed pool
    let runtime = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(num_worker_threads)
        .on_thread_start(move || {
            // Pin each thread to a CPU (CPUs 1 through N)
            static THREAD_COUNTER: AtomicUsize = AtomicUsize::new(0);
            let thread_id = THREAD_COUNTER.fetch_add(1, Ordering::SeqCst);

            let cpu_index = thread_id % worker_core_ids.len();
            let core_id = worker_core_ids[cpu_index];

            if core_affinity::set_for_current(core_id) {
                info!("Thread {} pinned to CPU {}", thread_id, cpu_index + 1);
            } else {
                log::warn!("Failed to pin thread {} to CPU {}", thread_id, cpu_index + 1);
            }
        })
        .enable_all()
        .build()
        .unwrap();

    runtime.block_on(async_main(args))?;
    Ok(())
}

async fn async_main(args: Args) -> Result<()> {

    assert!(
        args.msg_size() > size_of::<AppHdr>().try_into().unwrap(),
        "Message size is too small."
    );

    assert_eq!(machnet_init(), 0, "Failed to initialize Machnet library.");

    let mut channel = machnet_attach().unwrap();

    let ret = machnet_listen(&mut channel, args.server_ip(), args.port());
    assert_eq!(ret, 0, "Failed to listen on port {}", args.port());

    info!("[LISTENING] [{}:{}]", args.server_ip(), args.port());

    // Create multiple sandboxes and guest contexts
    info!("Initializing {} guests...", args.num_guests());
    let mut sandbox_creation_times: Vec<Duration> = Vec::new();

    let guest_contexts: Vec<Arc<Mutex<MultiUseGuestCallContext>>> = (0..args.num_guests())
        .map(|i| {
            info!("Initializing guest {}/{}", i + 1, args.num_guests());
            let start_time = Instant::now();
            let sandbox = init_plain_sandbox();
            let ctx = sandbox.new_call_context();
            let creation_time = start_time.elapsed();
            sandbox_creation_times.push(creation_time);
            debug!("Guest {} initialization took {:?}", i + 1, creation_time);
            Arc::new(Mutex::new(ctx))
        })
        .collect();

    // Report sandbox creation statistics
    if !sandbox_creation_times.is_empty() {
        let total: Duration = sandbox_creation_times.iter().sum();
        let avg = total / sandbox_creation_times.len() as u32;
        let min = sandbox_creation_times.iter().min().unwrap();
        let max = sandbox_creation_times.iter().max().unwrap();

        info!("All guests initialized successfully");
        info!("Sandbox creation statistics:");
        info!("  Total sandboxes: {}", sandbox_creation_times.len());
        info!("  Min time: {:?}", min);
        info!("  Max time: {:?}", max);
        info!("  Avg time: {:?}", avg);
        info!("  Total time: {:?}", total);
    }

    let comm_style = args.comm_style();
    let msg_size = args.msg_size();
    let msg_window = args.msg_window();

    // Create shutdown flag
    let shutdown_flag = Arc::new(AtomicBool::new(false));
    let shutdown_clone = Arc::clone(&shutdown_flag);

    // Set up signal handler
    tokio::spawn(async move {
        match tokio::signal::ctrl_c().await {
            Ok(()) => {
                info!("Signal received! Shutting down...");
                shutdown_clone.store(true, Ordering::SeqCst);
            }
            Err(err) => {
                eprintln!("Unable to listen for shutdown signal: {}", err);
            }
        }
    });

    // Run server
    server(guest_contexts, channel, msg_size, msg_window, comm_style, shutdown_flag).await;

    Ok(())
}
