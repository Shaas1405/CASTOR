
# CASTOR
### Communication and Autonomous System for Tactical Operations & Rescue  

**Problem** - Natural disasters can rapidly disable communication infrastructure, leaving affected communities and emergency responders unable to communicate  

**Goal** - Develop a portable, infrastructure-independent emergency communication network that allows people and responders to exchange critical information even when cellular and internet networks are unavailable.

## Core Concept
Castor links multiple independent, decentralized nodes over long-range radio frequencies. Every individual node acts as an active repeating router that can autonomously perform seven primary tasks:

* Receive packets from adjacent nodes
* Transmit self-generated payloads
* Forward external packets through the mesh
* Detect and drop duplicate traffic
* Decrease packet Time-to-Live (TTL) values
* Generate new localized telemetry or text data
* Connect nearby civilian devices through localized Wi-Fi portals

## Hardware Architecture
Each network node is constructed using commercial off-the-shelf hardware, ensuring low assembly costs and high deployability:

* Microcontroller: ESP32-S3 (Manages system logic, packet parsing, and web interface)
* LoRa Module: Waveshare Core1262-868M (Handles long-range, low-power radio communication)
* Antenna: High-gain 915Mhz tuned antenna
* Power Source: Independent battery array / solar backup
* Access Layer: Integrated 2.4GHz Wi-Fi access point for localized user discovery

## Network Routing & Mitigation Mechanics

### Time To Live (TTL)
To prevent infinite routing loops that saturate the airwaves, every packet drops its TTL value by 1 at every node jump. Once TTL is 0, the packet is dropped.

### Duplicate Packet Detection
Because mesh topologies naturally create multi-path echoes, a node can receive the exact same packet from different neighboring vectors. 

Every node retains a rolling lookup Packet Cache tracking the unique identifier signature, being the source ID + the packet's random ID
