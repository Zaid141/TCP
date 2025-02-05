
# Transmission Control Protocol (TCP) Project

## Overview
This project focuses on the implementation of a simplified version of the Transmission Control Protocol (TCP) from scratch. It consists of two tasks:

- **Task 1**: Simplified TCP sender/receiver with reliable data transfer.
- **Task 2**: Implementation of TCP congestion control.

## Requirements
Before running the project, ensure you have the following tools installed:

- A C compiler (e.g., GCC)
- Make utility

## Setup Instructions

### 1. Clone the Repository

To clone the repository, use the following command:

```bash
git clone https://github.com/your-username/TCP.git
cd TCP
```

### 2. Compile the Project

Once the repository is cloned, compile the necessary C files using the `make` command:

```bash
make
```

### 3. Start the Receiver

Navigate to the receiver directory and start the receiver program:

```bash
cd receiver
./receiver
```

### 4. Start the Sender

In a new terminal window, navigate to the sender directory and start the sender program:

```bash
cd sender
./sender
```

The sender will begin transmitting data to the receiver, and the data transfer process will follow the implementation of the TCP protocol.

## Features Implemented

### Task 1: Simplified TCP Sender/Receiver
- Reliable data transfer, with handling of packet loss and retransmission.
- Cumulative acknowledgments.
- Sliding window for packet management.
- Single retransmission timer.

### Task 2: TCP Congestion Control
- Slow-start mechanism.
- Congestion avoidance algorithm.
- Fast retransmit mechanism.

## License
This project is open-source under the MIT License.

