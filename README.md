# LED-control
Misc code for controlling LED strip lights

## spe6ctrl
Rather than build a custom controller, hacking a low-cost one seemed like a time saver. For as popular as these SP6xxE series controllers appear, the only obvious automation control project was from the [UniLED project](https://github.com/monty68/uniled). While too heavyweight for my needs and missing some important control elements, credit to the author for making it work and detailing the protocol. The controller design is simple with a chunk of queriable parameter memory and commands to change parameters. Read parameter memory, send a command, look for visible result, read parameter memory again for changes, and repeat until the controller has given up its secrets.

One very annyoing issue is querying parameter memory is problematic due to bluetooth issues. A single-response query (0x02 0x01) returns 47 valid bytes followed by 50 corrupt likely due to a PDU overrun. A multi-response query (0x02 0x00) returns 7 notifications of 14/13 bytes (97 total) but causes a disconnect 10-20% of the time likely due to poor controller implementation that sleeps between notifications rather than waiting for delivery (and disconnects on buffer overrun).

The 0x5e bulk-set command allow setting a mode (static, dynamic, sound or custom) and all its attributes in a single transaction. Command processing has been solid (no obvious disconnects) and single-transaction further increases reliability. 
