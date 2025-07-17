# DedupMQ
Mosquitto plugin written in C to prevent message loops in mesh server topology where everyone is connected to everyone.

# Why?
Having federated MQTT network where each server node is managed by different teams and exchanging the same 
topics in both directions might sound redundant and great for communication resilience.
Unfortunately that would cause message loops where all nodes will republish all messages they receive.

That's bad.

And it seems like it's not common practice to have such topology and most MQTT brokers don't have such protection built in.

# How?
DedupMQ will store payload hash in Memcashed storage for certain period of time (TTL).
All incoming messages are matched against this storage and if duplicate is found - the message is dropped.

# Compilation
First install the prerequisites:
```
apt update
apt install libmosquitto-dev libmemcached-dev libssl-dev mosquitto-dev gcc memcached
```
Then compile:
```
gcc -fPIC -shared dedupmq.c -lmosquitto -lmemcached -lssl -lcrypto -o /usr/lib/dedupmq.so
```

# Configuration & Installation
Add the following lines to Mosquitto's configuration:
```
plugin /usr/lib/dedupmq.so
plugin_opt_memcached_host 127.0.0.1
plugin_opt_memcached_port 11211
plugin_opt_ttl 60
plugin_opt_verbose_log 0
plugin_opt_topic msh/#
plugin_opt_topic sensor/+/data
```
Add as many as 64 topics to be followed and observed for loops.

Restart Mosquitto:
```
systemctl restart mosquitto
```

Verify the plugin is loaded:
Check Mosquitto's log (usually /var/log/mosquitto/mosquitto.log) for lines like this:
```
[dedupmq] Plugin initializing...
[dedupmq] Plugin loaded. Monitoring 1 topics.
```
