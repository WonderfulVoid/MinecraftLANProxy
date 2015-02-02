MinecraftLANProxy
==============

Purpose
--------------
MCLANProxy is a proxy server that enables remote hosts to connect to a Minecraft
LAN world. Minecraft unfortunately uses a dynamic port for the server connection
which makes it difficult to open up this port in e.g. router firewall.
MCLANProxy instead listens on a fixed port and forwards all connections and data
to the Minecraft LAN server.

I wrote this little hack because my daughter wanted her friends to be able to
join her Minecraft worlds even when not on the same LAN.

Usage
--------------
$ ./mclanproxy [-p {port}] [-v] [-V]
- -p {port}       Public port number
- -v verbose
- -V extra verbose

The public TCP port must be forwarded in the router firewall to the host running MCLANProxy. Remote Minecraft clients should connect to the public port (default 12345) on the router's public IP address.

If the remote Minecraft client immediately disconnects from the proxy (or from the LAN world server), this may depend on the client and server being of different incompatible versions. This happens too often when you run mods that require old Minecraft versions.

Limitations
--------------
- Only supports one LAN world at a time.

Design
--------------
MCLANProxy listens for Minecraft's LAN world announcements on IP multicast
address 224.0.2.60 UDP port 4445. When a LAN world has been detected,
MCLANProxy accepts TCP connections on the public port from remote Minecraft
clients. A proxy process is forked for each remote client.
The proxy creates a connection to the LAN server and forwards data in both
directions between the remote host and the LAN world.

Final Words
--------------
Possibly there is some better or simpler way of accomplishing this goal but it
was fun refreshing the old socket programming skills.
