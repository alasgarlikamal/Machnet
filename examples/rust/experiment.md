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





