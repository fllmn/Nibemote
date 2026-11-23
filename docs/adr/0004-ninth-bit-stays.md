# The ninth bit stays on the wire; the census goes

The ninth marking bit is wire fact: polls, ENQ and the closing ETX carry 1, frame bytes
carry 0, and a node that transmits everything 8N1 marks its bare ACKs as if they were ENQ,
which can make other nodes believe they were polled. The transport therefore keeps writing
the bit on transmit and keeps reading it on receive, where it almost determines a byte's
class for the parser. The parity-slot counters and flags existed to settle the hypothesis
and are deleted; the parity-error statistic stays as the transport's corruption signal.
