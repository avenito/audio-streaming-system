# Audio Streaming System
### General Description
A simple system based on mesh and audio streaming.
### Target
The target devices are:
1. Raspberry Pi for the central
2. ESP-32 for the nodes
### Basic Architecture
Figure 1 depict the basic architecture.
1. Central: This device runs the web interface and setup the network configuration.
2. Node: All nodes can connect one A2DP device, working as source, or can be connect to a digital analog converter.
<p align="center"><img src="Doc/BasicArchitecture.jpg" height="353px" width="560px" /></br>
<span>Figure 1</span>
</p>
