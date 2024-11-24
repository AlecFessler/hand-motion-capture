# Multi-Camera Hand Pose Dataset Collection System

This project aims to leverage Google's Mediapipe hand pose predictor, a custom multi-camera enclosure, and triangulation to bootstrap a 3D hand pose dataset at a rate of 108,000 labeled training samples per hour of recording. The ultimate goal is to design a neural net architecture for accurate, real-time, wearable-free hand motion capture.

### Architecture Diagram

```mermaid
flowchart TD
    classDef lightClass fill:#f5f9ff,color:black,stroke:#d3e3fd
    classDef medClass fill:#d3e3fd,color:black,stroke:#2b6cb0
    classDef darkClass fill:#2b6cb0,color:white
    classDef serverClass fill:#1a4971,color:white

    subgraph MCU["Microcontroller Layer"]
        AVR["AVR Timer (30Hz Pulse)"]
        GPIO["GPIO Lines"]
        AVR --> GPIO
    end

    GPIO --> |"Hardware Interrupt"| KM1 & KM2 & KM3

    subgraph RPi1["Raspberry Pi 1"]
        KM1["Kernel Module"] --> Cap1["Camera Handler"] --> Enc1["H.264 Encoder"]
    end

    subgraph RPi2["Raspberry Pi 2"]
        KM2["Kernel Module"] --> Cap2["Camera Handler"] --> Enc2["H.264 Encoder"]
    end

    subgraph RPi3["Raspberry Pi 3"]
        KM3["Kernel Module"] --> Cap3["Camera Handler"] --> Enc3["H.264 Encoder"]
    end

    subgraph Server["Server Layer"]
        Mgr["Manager Script"]
        Mgr --> FF1["FFmpeg Service 1"] & FF2["FFmpeg Service 2"] & FF3["FFmpeg Service 3"]
        Mgr --> WD1["Watchdog 1"] & WD2["Watchdog 2"] & WD3["Watchdog 3"]

        FF1 --> |"Listen"| TCP1["TCP Port 12345"]
        FF2 --> |"Listen"| TCP2["TCP Port 12346"]

        Enc1 --> TCP1
        Enc2 --> TCP2
        Enc3 --> TCP3

        FF1 --> |"Segments"| Seg1["Video Segments 1"]
        FF2 --> |"Segments"| Seg2["Video Segments 2"]
        FF3 --> |"Segments"| Seg3["Video Segments 3"]

        WD1 --> |"Monitor"| TCP1
        WD2 --> |"Monitor"| TCP2
        WD3 --> |"Monitor"| TCP3

        WD1 --> |"Done"| Pre1["Preprocessing 1"]
        WD2 --> |"Done"| Pre2["Preprocessing 2"]
        WD3 --> |"Done"| Pre3["Preprocessing 3"]

        Seg1 --> Pre1
        Seg2 --> Pre2
        Seg3 --> Pre3
    end

    class AVR,GPIO lightClass
    class KM1,KM2,KM3,Cap1,Cap2,Cap3 medClass
    class Enc1,Enc2,Enc3,FF1,FF2,FF3,TCP1,TCP2,TCP3 darkClass
    class Mgr,WD1,WD2,WD3,Pre1,Pre2,Pre3,Seg1,Seg2,Seg3 serverClass
```

### Physical Setup
[Photos/diagrams of recording frame and hardware]

## Technical Components

### Camera Synchronization

The system achieves sub-millisecond frame capture synchronization across multiple cameras using a precise hardware and software stack. An AVR microcontroller generates GPIO interrupts at 30 FPS using its onboard clock, which are processed by a custom Linux kernel module on each Raspberry Pi. The kernel module signals a registered userspace process, which handles the camera control. Precise timing is ensured through FIFO scheduling at maximum priority, running on a fully preemptable kernel (PREEMPT_RT patch). Camera timing determinism is further enhanced by disabling automatic adjustment algorithms and using fixed parameters for exposure, focus, and gain, made possible by the consistent lighting and known capture volume of the recording environment.

### Video Pipeline

The video pipeline is a single-threaded recording application driven by two signal handlers - a design that evolved from multi-threaded versions after finding that cross-core data sharing and additional synchronization complexity reduced performance. When triggered by the kernel module's GPIO interrupt, one handler queues a capture request to the camera. Upon DMA buffer completion, a second handler enqueues the buffer pointer into a lock-free queue for processing. The main loop dequeues these buffers, performs H.264 encoding, and streams the result via TCP to the server's FFmpeg instance. A semaphore tracking queue size prevents busy waiting in the main loop. The lock-free queue design is crucial, allowing the capture completion handler to safely preempt the main loop while preventing data races between components. Once the pipeline is running, timing between capture signal, capture completion, and frame transmission shows sub-millisecond variation, with approximately 28.3-28.7ms of idle time between frame transmission and the next capture signal at 33.33ms apart, validating the simplified single-threaded approach. A timer-based disconnect mechanism closes the TCP connection after 0.3 seconds without new frames, enabling the server to detect recording completion and prepare for the next session. To enable precise timing analysis, a custom async-signal-safe logger captures events directly within signal handlers. Sub-millisecond synchronization is verified through comparing timestamps from these logs across cameras, made possible by NTP synchronization of all Raspberry Pis to the central server.

### Server-Side Pipeline

The server-side pipeline takes advantage of FFmpeg's native support for handling incoming streams, eliminating the need for a conventional server application. The system uses systemd services to handle FFmpeg instances, with a separate service instance listening on a dedicated port for each incoming camera stream. Incoming H.264 streams are recorded using FFmpeg's -segment feature, which creates new files for each segment within a session to allow seamless recovery after interruptions. The streams are transcoded server-side to H.265, leveraging GPU acceleration for efficient compression while minimizing computational load on the Raspberry Pis.

A manager script provides commands to start, stop, restart, and monitor all services. For each port, it launches both an FFmpeg service instance and a dedicated watchdog process. The manager uses a simple counter stored in a dotfile to enumerate recording sessions, ensuring unique filenames across sessions. Each watchdog monitors its assigned port, waiting for the creation of an initial segment to confirm that recording has started. Once detected, it monitors progress by using inotify on the latest segment, avoiding false triggers caused by FFmpeg restarting and creating a new segment. When recording ends on a port, its watchdog executes port-specific preprocessing as a background task, allowing the system to immediately prepare for the next recording session.

### Calibration Pipeline
- Lens distortion correction
- Camera alignment
- Stereo calibration
- Frame validation

## Hardware Setup

### Components
- Camera specifications
- Frame construction
- GPIO wiring
- ArUco markers

### Assembly
- Frame assembly
- Camera mounting
- Electronics installation
- Calibration markers

## Dataset Generation

### Pipeline Overview
- Video preprocessing
- MediaPipe integration
- Multi-view triangulation
- Data format

### Output Format
- Dataset structure
- File formats
- Sample counts
- Data fields

## Results & Examples
[Visual examples of system output, calibration results, etc.]

## License
This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

## Acknowledgments
Any credits or acknowledgments
