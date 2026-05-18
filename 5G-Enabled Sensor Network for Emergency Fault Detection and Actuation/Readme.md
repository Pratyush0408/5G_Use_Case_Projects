# 5G-Enabled Sensor Network for Emergency Fault Detection and Actuation

## Overview
This project presents a comprehensive, 5G-enabled Internet of Things (IoT) sensor network specifically engineered for rapid emergency fault detection and automated actuation. Traditional cloud-based IoT architectures often suffer from variable, high latency, which can be detrimental in safety-critical scenarios. To solve this, our system connects a sensor node, an actuator node, and a responsive web-based MQTT dashboard through a Mobile Edge Computing (MEC) server running a local Mosquitto MQTT broker. 

By routing all network traffic exclusively through an MEC server via an industrial RUTX11 cellular gateway, the system keeps data at the edge rather than sending it over the public internet. This localized approach achieves a highly stable, mean round-trip latency of approximately 30 ms. Consequently, this architecture is vastly superior and highly suited for latency-sensitive applications where immediate hardware response is required to prevent damage or ensure safety.

## Objectives
* **Real-Time Environmental and Electrical Monitoring:** Design and implement a robust multi-node IoT system capable of real-time, continuous sensing of ambient temperature, open flames, and electrical circuit voltage.
* **Edge-Optimized Connectivity:** Deploy a Mosquitto MQTT broker on an MEC server and ensure all nodes communicate exclusively through a 5G/LTE cellular gateway to exploit the low-latency advantages of edge computing.
* **Autonomous Safety Actuation:** Implement priority-based fault detection logic. Upon detecting critical events (such as an open flame or severe temperature threshold breaches), the actuator node must autonomously cut circuit power within a single MQTT message round-trip to prevent system damage.
* **Interactive Telemetry Interface:** Develop a responsive, user-friendly web dashboard that provides live telemetry visualization and allows users to remotely execute manual overrides for the system's relays and actuators.
* **Performance Benchmarking:** Quantitatively benchmark the latency of the MEC-hosted MQTT broker against a standard public cloud MQTT broker (HiveMQ) to demonstrate the statistical and practical advantages of edge computing in emergency systems.

## System Architecture
The system is logically divided into three interconnected layers, ensuring a clean separation of concerns:
* **Perception Layer (Sensing):** Powered by the Sensor ESP32 node. This layer is responsible for interfacing with the physical environment, gathering raw data from the connected sensors, and publishing structured MQTT telemetry messages to the broker.
* **Actuation Layer (Control):** Powered by the Actuator ESP32 node. This layer subscribes to the relevant MQTT topics, processes the incoming sensor data or manual commands, and physically drives the protective relays and a PWM-controlled cooling fan.
* **Application Layer (Visualization):** The MEC MQTT Dashboard. This layer acts as the central interface, subscribing to all system topics to display live data and providing a control panel for operators to publish manual-control commands back to the actuator node.

## Hardware Components

### Sensor Node
* **Microcontroller:** Built around an ESP32 development board mounted on a half-size breadboard for rapid prototyping.
* **Temperature Monitoring:** Features a DS18B20 Digital Temperature Sensor. This component continuously monitors the thermal state of the environment and dictates the dynamic speed zones for the cooling fan.
* **Fire Detection:** Includes an Infrared (IR) Flame Sensor Module. This highly sensitive component produces a digital LOW output upon detecting a flame, triggering an immediate, high-priority fault-latch sequence in the system.
* **Voltage Telemetry:** Utilizes a custom resistive voltage divider paired with the ESP32's internal Analog-to-Digital Converter (ADC). The readings are mapped and calibrated through software to accurately monitor the circuit's supply voltage.

### Actuator Node
* **Microcontroller & Drivers:** Uses a secondary ESP32 board to govern four separate relay channels via two L298N dual H-bridge motor driver modules, which provide necessary current isolation.
* **Motor Control (Channels 1 & 2):** These relays switch power to the primary motor load. For safety, these channels are strictly inhibited (locked out) during an active flame-latch event.
* **Emergency Cut-Off (Channels 3 & 4):** These relays act as the main voltage supply cut-off. They are programmed to activate automatically and instantly upon flame detection or if temperature limits are exceeded.
* **Thermal Management:** Features a 5V DC fan driven by a precise Pulse Width Modulation (PWM) signal. The fan speed scales automatically based on the temperature zones detected by the sensor node, but can also be manually overridden by an operator.

## Software Design

* **Sensor Node Firmware:** Developed using the Arduino/ESP-IDF framework. The firmware handles the initialization of all peripheral sensors, samples the ADC and digital pins at a high frequency, and efficiently serializes the readings into plain-text payloads published via the PubSubClient MQTT library.
* **Actuator Node Firmware:** Engineered for safety and reliability. It subscribes to the telemetry topics and executes a strict, priority-ordered fault handler. For example, if a flame is detected, the firmware immediately sets a local "flame-latch" flag, forcefully opens the voltage cut-off relays, logically inhibits the motor relays to prevent re-engagement, and ramps the cooling fan to maximum power.
* **MEC MQTT Dashboard:** A modern, single-page web application served over HTTP from a lightweight Node.js server co-located on the MEC host. It communicates with the Mosquitto broker using WebSockets, providing near-instantaneous live telemetry updates (flame status, temperature gauges, voltage charts) alongside an intuitive manual control panel for toggling relays and adjusting fan speeds.

## Results and Performance

* **Edge Latency:** The localized, MEC-hosted Mosquitto broker delivered an exceptionally stable mean round-trip latency of approximately **30 ms**.
* **Cloud Latency:** In stark contrast, benchmark testing with a public HiveMQ cloud broker yielded a mean latency of roughly **8666 ms** (over 8.6 seconds), suffering from massive variance and high jitter due to internet routing overhead.
* **Conclusion:** The MEC-based approach demonstrated a staggering latency reduction of approximately **290x** compared to the public cloud alternative. This firmly proves that edge-hosted architectures are not just beneficial, but fundamentally necessary for safety-critical actuation where every millisecond counts.

## Academic Information
* **Institution:** Indian Institute of Technology Guwahati
* **Course:** EE 396 Design Lab
* **Authors:** Dev Garg (Roll Number: 230102026) and Pratyush Kumar Jha (Roll Number: 230102074)
* **Supervisor:** Dr. Salil Kashyap
