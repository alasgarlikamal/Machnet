use anyhow::Result;

pub struct Args {
    server_ip: String,
    guest: String,
    init_sandbox_size: usize,
    msg_size: u64,
    msg_window: u64,
    port: u16,
    num_guests: usize,
    communication_style: String,
}

impl Args {
    const OPT_HELP: &'static str = "-help";
    const OPT_SERVER_IP: &'static str = "-listen";
    const OPT_GUEST: &'static str = "-guest";
    const OPT_INIT_SANDBOX_SIZE: &'static str = "-init-sandbox-size";
    const OPT_MSG_SIZE: &'static str = "-msg-size";
    const OPT_PORT: &'static str = "-port";
    const OPT_MSG_WINDOW: &'static str = "-msg-window";
    const OPT_NUM_GUESTS: &'static str = "-num-guests";
    const OPT_COMM_STYLE: &'static str = "-comm-style";
    pub fn parse(args: Vec<String>) -> Result<Self> {
        let mut server_ip: String = String::new();
        let mut guest: String = String::new();
        let mut init_sandbox_size: usize = 0;
        let mut msg_size: u64 = 0;
        let mut port: u16 = 0;
        let mut msg_window: u64 = 0;
        let mut num_guests: usize = 1;
        let mut communication_style: String = "round-robin".to_string();
        let mut i: usize = 1;
        while i < args.len() {
            match args[i].as_str() {
                Self::OPT_HELP => {
                    Self::usage(args[0].as_str());
                    return Err(anyhow::anyhow!("wrong usage"));
                }
                Self::OPT_SERVER_IP => {
                    i += 1;
                    server_ip = args[i].clone();
                }
                Self::OPT_GUEST => {
                    i += 1;
                    guest = args[i].clone();
                }
                Self::OPT_INIT_SANDBOX_SIZE => {
                    i += 1;
                    init_sandbox_size = args[i].parse::<usize>().unwrap();
                }
                Self::OPT_MSG_SIZE => {
                    i += 1;
                    msg_size = args[i].parse::<u64>().unwrap();
                }
                Self::OPT_PORT => {
                    i += 1;
                    port = args[i].parse::<u16>().unwrap();
                }
                Self::OPT_MSG_WINDOW => {
                    i += 1;
                    msg_window = args[i].parse::<u64>().unwrap();
                }
                Self::OPT_NUM_GUESTS => {
                    i += 1;
                    num_guests = args[i].parse::<usize>().unwrap();
                }
                Self::OPT_COMM_STYLE => {
                    i += 1;
                    communication_style = args[i].clone();
                }
                _ => {
                    return Err(anyhow::anyhow!("invalid argument"));
                }
            }

            i += 1;
        }

        Ok(Self {
            server_ip,
            guest,
            init_sandbox_size,
            msg_size,
            port,
            msg_window,
            num_guests,
            communication_style,
        })
    }

    pub fn usage(program_name: &str) {
        println!(
            "Usage: {} {} <sockaddr> {} <filepath> {} <init-sandbox-size> {} <msg-size> {} <port> {} <msg-window> {} <num-guests> {} <comm-style>",
            program_name,
            Self::OPT_SERVER_IP,
            Self::OPT_GUEST,
            Self::OPT_INIT_SANDBOX_SIZE,
            Self::OPT_MSG_SIZE,
            Self::OPT_PORT,
            Self::OPT_MSG_WINDOW,
            Self::OPT_NUM_GUESTS,
            Self::OPT_COMM_STYLE,
        );
        println!("  Communication styles: round-robin, random, broadcast");
    }

    pub fn server_ip(&self) -> &str {
        &self.server_ip
    }

    pub fn guest(&self) -> &str {
        &self.guest
    }

    pub fn init_sandbox_size(&self) -> usize {
        self.init_sandbox_size
    }

    pub fn msg_size(&self) -> u64 {
        self.msg_size
    }

    pub fn port(&self) -> u16 {
        self.port
    }

    pub fn msg_window(&self) -> u64 {
        self.msg_window
    }

    pub fn num_guests(&self) -> usize {
        self.num_guests
    }

    pub fn communication_style(&self) -> &str {
        &self.communication_style
    }
}
