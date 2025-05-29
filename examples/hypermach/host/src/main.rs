mod args;
mod stats;

use std::{
    env,
    sync::atomic::{AtomicBool, Ordering},
    thread,
    time::Duration,
};

use crate::args::Args;

use ::anyhow::Result;
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
use signal_hook::{consts::SIGINT, iterator::Signals};
use stats::{Stats, report_stats};

struct AppHdr {
    window_slot: u64,
}

// Flag to keep the server running
static G_KEEP_RUNNING: AtomicBool = AtomicBool::new(true);

// Machnet constants
const MACHNET_MSG_MAX_LEN: usize = 64; // 8 * 1 << 20; // 8MB

// Hyperlight guest binary path
const GUEST_PATH: &str = "/home/vj2267/machnet/examples/hypermach/guest/target/x86_64-unknown-none/release/hypermach-guest";

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
    mut guest_ctx: MultiUseGuestCallContext,
    mut channel: MachnetChannel,
    msg_size: u64,
    msg_window: u64,
) {
    let _ = msg_window;
    info!("Server: Starting...");

    let mut stats = Stats::new();

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

        // Do a guest function call
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
            hyperlight_host::func::ReturnValue::VecBytes(tx_message) => {
                assert_eq!(tx_message, rx_message, "Echo output does not match input");
            }
            _ => panic!("Unexpected return type from DirectEcho function"),
        }

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
    }
}

fn main() -> Result<()> {
    // unsafe { env::set_var("RUST_LOG", "info") };
    // env_logger::init();

    let args: Args = Args::parse(std::env::args().collect())?;

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

    let sandbox = init_plain_sandbox();
    let guest_ctx = sandbox.new_call_context();

    let datapath_thread =
        thread::spawn(move || server(guest_ctx, channel, args.msg_size(), args.msg_window()));

    while G_KEEP_RUNNING.load(Ordering::SeqCst) {
        thread::sleep(Duration::from_secs(5));
    }

    datapath_thread.join().unwrap();
    Ok(())
}
