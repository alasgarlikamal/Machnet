## Running channel scalability experiment
Each client <-> server connection opens server <-> machnet and client <-> machnet channels

N: number of channels to test

 - server side spawns N servers each with different port - you can do 1000 + N
 - client side spawns N clients each with binding to port 1000 + N
    client runs for t seconds (default to 10 seconds)
    client output is in the form of 
    ``` 
    [2025-05-28T20:44:43Z INFO  msg_gen] TX/RX (msg/sec, Gbps): (751.4K/751.4K, 0.385/0.385). RTT (p50/99/99.9 us): 9/26/31
    ```
    save it to the csv file where columns are TX msg/sec, RX msg/sex, TX Gbps, RX Gbps, RTT p50 us, RTT p99 us, RT p99.9 us
    and the file name is  client_{date/timestamp}_{1..N}.csv
    at the end make aggregate file client_{date/timestamp}_combined_N.csv where TX, RX fields are summed and RTT fields are averaged


Warning:
 - be careful, now it is all average. IMHO, arithmetic average over the runs of the same client and geometric average over clients is the way to go!


## Experiment Run

Instance type is `sm110p`.

__TLDR__: 2 hyperthreds on 16 cores = 32 logical cores

```
Architecture:             x86_64
  CPU op-mode(s):         32-bit, 64-bit
  Address sizes:          46 bits physical, 57 bits virtual
  Byte Order:             Little Endian
CPU(s):                   32
  On-line CPU(s) list:    0-31
Vendor ID:                GenuineIntel
  Model name:             Intel(R) Xeon(R) Silver 4314 CPU @ 2.40GHz
    CPU family:           6
    Model:                106
    Thread(s) per core:   2
    Core(s) per socket:   16
    Socket(s):            1
    Stepping:             6
    CPU max MHz:          3400.0000
    CPU min MHz:          800.0000
    BogoMIPS:             4800.00
``` 

32 channels are problematic. Only 29 out of 32 channels succeeded. Need to investigate channel creation overheads + channel memory requirements. 

## Channel related data
- Maximum burst size: 64 messages (MsgBufBatch::kMaxBurst)
- Default ring size: 256 slots
- Default buffer count: 4096
- Maximum channel number: 32


