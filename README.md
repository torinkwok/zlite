zlite
=====

**fast and lightweight compression utility.**

zlite is a fast and lightweight lossless data compression utility. zlite implements ROLZ algorithm and POLAR encoding.

in practical, zlite compresses better and faster than gzip, but decompresses slower.

simple banchmark with __enwik7__ (10,000,000 bytes):

<table border="1">
 <tr><td>zlite</td> <td>3530440</td> <td>1.45s</td> <td>0.61s</td></tr>
 <tr><td>gzip</td>  <td>3693800</td> <td>1.90s</td> <td>0.24s</td></tr>
</table>
