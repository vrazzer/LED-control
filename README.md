# LED-control
Misc code for controlling LED strip lights

## spe6ctrl
Rather than build a custom controller, hacking an low-cost one seemed like a time saver. For as popular as these SPE6xx series controllers appear, the only automation control project I found was from the [UniLED project](https://github.com/monty68/uniled). It was too heavyweight for my needs and missing some important control elements, but credit to the author for making it work. The controller design is quite simple with a chunk of parameter memory that can be queried and a command set to change parameters. Read parameter memory, send a command, look for visible result, read parameter memory again to see what changed, and repeat until the controller has given up its secrets.

One very annyoing issue is that querying the parameter memory is flakey due to bluetooth issues. Querying all the memory in single transaction returns only 47 valid bytes and then corruption likely due to a PDU overrun. Querying via multiple short transactions fails 10-20% of the time likely due to a simplistic controller implementation that sleeps between notifications rather than waiting for delivery (and then disconents on buffer overrun).
