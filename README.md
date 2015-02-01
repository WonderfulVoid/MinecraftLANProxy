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
$ ./mclanproxy [-i {ifname}] [-p {port}] [-v]
- -i {ifname}     Interface to listen for announcements on
- -p {port}       Public port number
- -v verbose
- -V extra verbose

The public TCP port must be forwarded in the router firewall to the host running MCLANProxy. Remote Minecraft clients should connect to the public port (defaul 12345) on the router's public IP address.

If the remote Minecraft client immediately disconnects from the proxy (or from the LAN world server), this may depend on the client and server being of different incompatible versions. This happens too often when you run mods that require old Minecraft versions.

Limitations
--------------
- Must specify interface to listen for multicast announcements on unless it matches hardcoded default ("eth0"). Ideally a suitable Ethernet-like interface should automatically be found.
- MCLANProxy doesn't stop accepting remote connections when the LAN world announcement ceases.

Design
--------------
MCLANProxy listens for Minecraft's LAN world announcements (on IP multicast
address 224.0.2.60 UDP port 4445). When a LAN world has been detected, MCLANProxy accepts
remote TCP connections on the public port. A corresponding connection to the LAN
world is created and data is forwarded between the remote host and the LAN world
in both directions. The public port (together with the IP address of the
host MACLANProxy is running on) can be opened up in a router firewall.

Final Words
--------------
Possibly there is some better or simpler way of accomplishing this goal but it
was fun refreshing the old socket programming skills.
