MinecraftLANProxy
==============

Purpose
--------------
MCLANProxy is a proxy server that enables remote hosts to connect to a Minecraft
LAN world. Minecraft unfortunately uses a dynamic port for the server connection
which makes it difficult to open up this port in e.g. router firewall.
MCLANProxy instead listens on a fixed port and forwards all connections and data
to the temporary Minecraft LAN server.

I wrote this little hack because my daughter wanted her friends to be able to
join her Minecraft worlds even when not on the same LAN.

Design
--------------
MCLANProxy listens for Minecraft's LAN world announcements (on IP multicast
address 224.0.2.60). When a LAN world has been detected, MCLANProxy accepts
remote TCP connections on an defined port. A corresponding connection to the LAN
world is created and data is forwarded between the remote host and the LAN world
in both directions. The defined port (together with the IP address of the
host MACLANProxy is running on) can be opened up in a router firewall.

Usage
--------------
$ ./mclanproxy [-v] [-i <ifname>]
- -v verbose
- -i <ifname>     Specify interface to listen for announcements on

Limitations
--------------
- Interface to listen for multicast announcements on must be specified unless it matches hardcoded default ("eth0"). Ideally a suitable Ethernet-like interface should automatically be found.
- The public port for remote hosts is hardcoded to 12345.
- MCLANProxy doesn't stop accepting remote connections when the LAN world announcement ceases.

Final Words
--------------
Possibly there is some better or simpler way of accomplishing this goal but it
was fun refreshing the old socket programming skills.
