# CASTOR
Communication and Autonomous System for Tactical Operations & Rescue  
Problem - Natural disasters can rapidly disable communication infrastructure, leaving affected communities and emergency responders unable to communicate  
Goal - Develop a portable, infrastructure-independent emergency communication network that allows people and responders to exchange critical information even when cellular and internet networks are unavailable.

Core Concept  

Castor consists of multiple independent nodes that communicate with one another using LoRa.  

Each node can:  

Receive packets  
Transmit packets  
Forward packets from other nodes  
Detect duplicate packets  
Decrease packet TTL values  
Generate new packets  
Connect nearby users through Wi-Fi  

A message does not need to travel directly from the sender to its destination.  

Instead, it can travel through multiple nodes.  
